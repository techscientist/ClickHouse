#include <DB/IO/ReadBufferAIO.h>
#include <DB/Common/ProfileEvents.h>
#include <DB/Core/ErrorCodes.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace DB
{

ReadBufferAIO::ReadBufferAIO(const std::string & filename_, size_t buffer_size_, int flags_, mode_t mode_,
	char * existing_memory_)
	: BufferWithOwnMemory(buffer_size_, existing_memory_, BLOCK_SIZE),
	fill_buffer(BufferWithOwnMemory(buffer_size_, existing_memory_, BLOCK_SIZE)),
	filename(filename_)
{
	ProfileEvents::increment(ProfileEvents::FileOpen);

	int open_flags = ((flags_ == -1) ? O_RDONLY : flags_);
	open_flags |= O_DIRECT;

	fd = open(filename_.c_str(), open_flags, mode_);
	if (fd == -1)
		throwFromErrno("Cannot open file " + filename_, errno == ENOENT ? ErrorCodes::FILE_DOESNT_EXIST : ErrorCodes::CANNOT_OPEN_FILE);
}

ReadBufferAIO::~ReadBufferAIO()
{
	try
	{
		waitForCompletion();
	}
	catch (...)
	{
		tryLogCurrentException(__PRETTY_FUNCTION__);
	}

	close(fd);
}

std::string ReadBufferAIO::getFileName() const
{
	return filename;
}

bool ReadBufferAIO::nextImpl()
{
	waitForCompletion();

	// Create request.
	memset(&cb, 0, sizeof(cb));

	cb.aio_lio_opcode = IOCB_CMD_PREAD;
	cb.aio_fildes = fd;
	cb.aio_buf = reinterpret_cast<UInt64>(this->buffer().begin());
	cb.aio_nbytes = this->buffer().size();
	cb.aio_offset = 0;
	cb.aio_reqprio = 0;

	swapBuffers();

	// Submit request.
	while (io_submit(aio_context.ctx, request_ptrs.size(), &request_ptrs[0]) < 0)
		if (errno != EINTR)
			throw Exception("Cannot submit request for asynchronous IO", ErrorCodes::AIO_SUBMIT_ERROR);

	is_pending_read = true;

	return true;
}

void ReadBufferAIO::waitForCompletion()
{
	if (is_pending_read)
	{
		memset(&events[0], 0, sizeof(events[0]) * events.size());

		while (io_getevents(aio_context.ctx, events.size(), events.size(), &events[0], nullptr) < 0)
			if (errno != EINTR)
				throw Exception("Failed to wait for asynchronous IO completion", ErrorCodes::AIO_COMPLETION_ERROR);

		fill_buffer.buffer().resize(events[0].res);
		is_pending_read = false;
	}
}

void ReadBufferAIO::swapBuffers()
{
	std::swap(this->buffer(), fill_buffer.buffer());
	std::swap(this->position(), fill_buffer.position());
}

}