#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <lib/base/rawfile.h>
#include <lib/base/eerror.h>

DEFINE_REF(eRawFile);

eRawFile::eRawFile()
	:m_lock(false)
{
	m_fd = -1;
	m_file = 0;
	m_splitsize = 0;
	m_totallength = 0;
	m_current_offset = 0;
	m_base_offset = 0;
	m_last_offset = 0;
	m_nrfiles = 0;
	m_current_file = 0;
}

eRawFile::~eRawFile()
{
	close();
}

int eRawFile::open(const char *filename, int cached)
{
	close();
	m_cached = cached;
	m_basename = filename;
	scan();
	m_current_offset = 0;
	m_last_offset = 0;
	if (!m_cached)
	{
		m_fd = ::open(filename, O_RDONLY | O_LARGEFILE);
		return m_fd;
	} else
	{
		m_file = ::fopen64(filename, "rb");
		if (!m_file)
			return -1;
		return 0;
	}
}

void eRawFile::setfd(int fd)
{
	close();
	m_cached = 0;
	m_nrfiles = 1;
	m_fd = fd;
}

off_t eRawFile::lseek(off_t offset, int whence)
{
	eSingleLocker l(m_lock);
	m_current_offset = lseek_internal(offset, whence);
	return m_current_offset;
}

off_t eRawFile::lseek_internal(off_t offset, int whence)
{
//	eDebug("lseek: %lld, %d", offset, whence);
		/* if there is only one file, use the native lseek - the file could be growing! */
	if (m_nrfiles < 2)
	{
		if (!m_cached)
			return ::lseek(m_fd, offset, whence);
		else
		{
			if (::fseeko(m_file, offset, whence) < 0)
				perror("fseeko");
			return ::ftello(m_file);
		}
	}
	switch (whence)
	{
	case SEEK_SET:
		m_current_offset = offset;
		break;
	case SEEK_CUR:
		m_current_offset += offset;
		break;
	case SEEK_END:
		m_current_offset = m_totallength + offset;
		break;
	}

	if (m_current_offset < 0)
		m_current_offset = 0;
	return m_current_offset;
}

int eRawFile::close()
{
	if (m_cached)
	{
		if (!m_file)
			return -1;
		::fclose(m_file);
		m_file = 0;
		return 0;
	} else
	{
		int ret = ::close(m_fd);
		m_fd = -1;
		return ret;
	}
}

ssize_t eRawFile::read(off_t offset, void *buf, size_t count)
{
	eSingleLocker l(m_lock);

	if (offset != m_current_offset)
	{
		m_current_offset = lseek_internal(offset, SEEK_SET);
		if (m_current_offset < 0)
			return m_current_offset;
	}

	switchOffset(m_current_offset);

	if (m_nrfiles >= 2)
	{
		if (m_current_offset + count > m_totallength)
			count = m_totallength - m_current_offset;
		if (count < 0)
			return 0;
	}
	
	int ret;
	
	if (!m_cached)
		ret = ::read(m_fd, buf, count);
	else
		ret = ::fread(buf, 1, count, m_file);

	if (ret > 0)
		m_current_offset = m_last_offset += ret;
	return ret;
}

int eRawFile::valid()
{
	if (!m_cached)
		return m_fd != -1;
	else
		return !!m_file;
}

void eRawFile::scan()
{
	m_nrfiles = 0;
	m_totallength = 0;
	while (m_nrfiles < 1000) /* .999 is the last possible */
	{
		if (!m_cached)
		{
			int f = openFileUncached(m_nrfiles);
			if (f < 0)
				break;
			if (!m_nrfiles)
				m_splitsize = ::lseek(f, 0, SEEK_END);
			m_totallength += ::lseek(f, 0, SEEK_END);
			::close(f);
		} else
		{
			FILE *f = openFileCached(m_nrfiles);
			if (!f)
				break;
			::fseeko(f, 0, SEEK_END);
			if (!m_nrfiles)
				m_splitsize = ::ftello(f);
			m_totallength += ::ftello(f);
			::fclose(f);
		}
		
		++m_nrfiles;
	}
//	eDebug("found %d files, splitsize: %llx, totallength: %llx", m_nrfiles, m_splitsize, m_totallength);
}

int eRawFile::switchOffset(off_t off)
{
	if (m_splitsize)
	{
		int filenr = off / m_splitsize;
		if (filenr >= m_nrfiles)
			filenr = m_nrfiles - 1;
		if (filenr != m_current_file)
		{	
//			eDebug("-> %d", filenr);
			close();
			if (!m_cached)
				m_fd = openFileUncached(filenr);
			else
				m_file = openFileCached(filenr);
			m_last_offset = m_base_offset = m_splitsize * filenr;
			m_current_file = filenr;
		}
	} else
		m_base_offset = 0;
	
	if (off != m_last_offset)
	{
		if (!m_cached)
			m_last_offset = ::lseek(m_fd, off - m_base_offset, SEEK_SET) + m_base_offset;
		else
		{
			::fseeko(m_file, off - m_base_offset, SEEK_SET);
			m_last_offset = ::ftello(m_file) + m_base_offset;
		}
		return m_last_offset;
	} else
	{
		return m_last_offset;
	}
}

/* m_cached */
FILE *eRawFile::openFileCached(int nr)
{
	std::string filename = m_basename;
	if (nr)
	{
		char suffix[5];
		snprintf(suffix, 5, ".%03d", nr);
		filename += suffix;
	}
	return ::fopen64(filename.c_str(), "rb");
}

/* !m_cached */
int eRawFile::openFileUncached(int nr)
{
	std::string filename = m_basename;
	if (nr)
	{
		char suffix[5];
		snprintf(suffix, 5, ".%03d", nr);
		filename += suffix;
	}
	return ::open(filename.c_str(), O_RDONLY | O_LARGEFILE);
}

off_t eRawFile::length()
{
	return m_totallength;
}

#define KILOBYTE(n) ((n) * 1024)
#define MEGABYTE(n) ((n) * 1024LL * 1024LL)

eDecryptRawFile::eDecryptRawFile()
{
	ringBuffer = new cRingBufferLinear(MEGABYTE(2),TS_SIZE,true,"IN-TS");
	ringBuffer->SetTimeouts(100,100);
	bs_size = dvbcsa_bs_batch_size();
	delivered=false;
	lastPacketsCount = 0;
}

eDecryptRawFile::~eDecryptRawFile()
{
	delete ringBuffer;
}

void eDecryptRawFile::setDemux(ePtr<eDVBDemux> _demux) {
	demux = _demux;
}

uint8_t* eDecryptRawFile::getPackets(int &packetsCount) {
	int Count=0;
	if(delivered) {
		ringBuffer->Del(lastPacketsCount*TS_SIZE);
		delivered=false;
	}
	packetsCount = 0;

	if (ringBuffer->Available()<bs_size*TS_SIZE)
		return NULL;

	uint8_t* p=ringBuffer->Get(Count);
	if (Count>KILOBYTE(64))
		Count = KILOBYTE(64);

	if(p && Count>=TS_SIZE) {
		if(*p!=TS_SYNC_BYTE) {
			for(int i=1; i<Count; i++)
				if(p[i]==TS_SYNC_BYTE &&
						(i+TS_SIZE==Count || (i+TS_SIZE>Count && p[i+TS_SIZE]==TS_SYNC_BYTE)) ) {
					Count=i;
					break;
				}
				ringBuffer->Del(Count);
				eDebug("ERROR: skipped %d bytes to sync on TS packet", Count);
				return NULL;
		}
		if(!demux->decrypt(p, Count, packetsCount)) {
			cCondWait::SleepMs(20);
			return NULL;
		}
		lastPacketsCount = packetsCount;
		delivered=true;
		return p;
	}

	return NULL;
}

ssize_t eDecryptRawFile::read(off_t offset, void *buf, size_t count)
{
	eSingleLocker l(m_lock);
	int ret;

	while (ringBuffer->Available()<KILOBYTE(128)) {
		ret = ringBuffer->Read(m_fd, KILOBYTE(64));
		if (ret<0)
			break;
	}

	int packetsCount = 0;

	uint8_t *data = getPackets(packetsCount);

	//printf("getPackets(packetsCount) %d\n", packetsCount);

	if (data && packetsCount>0) {
		ret = packetsCount*TS_SIZE;
		memcpy(buf, data, ret);
	} else
		ret = EBUSY;

	return ret;
}

