#include "io_adapter.h"

#ifdef _WIN32
#  define NOMINMAX
#  define ssize_t int64_t
#  include <winsock2.h>
#  include <io.h>
#else
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#include <cstdio>
#include <cassert>
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>

#include <zlib.h>
#if (ZLIB_VERNUM < 0x1252)
typedef void* gzFile;
#else
struct gzFile_s;
typedef struct gzFile_s* gzFile;
#endif

#ifndef O_LARGEFILE  // for OSX
#  define O_LARGEFILE 0
#endif

using namespace VW::io;

enum class file_mode
{
  read,
  write
};

struct socket_adapter : public writer, public reader
{
  socket_adapter(int fd, const std::shared_ptr<details::socket_closer>& closer)
      : reader(false /*is_resettable*/), _socket_fd{fd}, _closer{closer}
  {
  }
  ssize_t read(char* buffer, size_t num_bytes) override;
  ssize_t write(const char* buffer, size_t num_bytes) override;

private:
  int _socket_fd;
  std::shared_ptr<details::socket_closer> _closer;
};

struct stdio_adapter : public writer, public reader
{
  stdio_adapter() : reader(false /*is_resettable*/) {}
  ssize_t read(char* buffer, size_t num_bytes) override;
  ssize_t write(const char* buffer, size_t num_bytes) override;
};
struct file_adapter : public writer, public reader
{
  // investigate whether not using the old flags affects perf. Old claim:
  // _O_SEQUENTIAL hints to OS that we'll be reading sequentially, so cache aggressively.
  file_adapter(const char* filename, file_mode mode);
  file_adapter(int file_descriptor, file_mode mode);
  ~file_adapter();
  ssize_t read(char* buffer, size_t num_bytes) override;
  ssize_t write(const char* buffer, size_t num_bytes) override;
  void reset() override;

private:
  int _file_descriptor;
  file_mode _mode;
};

struct gzip_file_adapter : public writer, public reader
{
  gzip_file_adapter(const char* filename, file_mode mode);
  gzip_file_adapter(int file_descriptor, file_mode mode);
  ~gzip_file_adapter();

  ssize_t read(char* buffer, size_t num_bytes) override;
  ssize_t write(const char* buffer, size_t num_bytes) override;
  void reset() override;

private:
  gzFile _gz_file;
  file_mode _mode;
};

struct gzip_stdio_adapter : public writer, public reader
{
  gzip_stdio_adapter();
  ~gzip_stdio_adapter();
  ssize_t read(char* buffer, size_t num_bytes) override;
  ssize_t write(const char* buffer, size_t num_bytes) override;

private:
  gzFile _gz_stdin;
  gzFile _gz_stdout;
};

struct vector_writer : public writer
{
  vector_writer(std::shared_ptr<std::vector<char>>& buffer);
  ~vector_writer() = default;
  ssize_t write(const char* buffer, size_t num_bytes) override;

private:
  std::shared_ptr<std::vector<char>> _buffer;
};

struct buffer_view : public reader
{
  buffer_view(const char* data, size_t len);
  ~buffer_view() = default;
  ssize_t read(char* buffer, size_t num_bytes) override;
  void reset() override;

private:
  const char* _data;
  const char* _read_head;
  size_t _len;
};

namespace VW
{
namespace io
{
std::unique_ptr<writer> open_file_writer(const std::string& file_path)
{
  return std::unique_ptr<writer>(new file_adapter(file_path.c_str(), file_mode::write));
}

std::unique_ptr<reader> open_file_reader(const std::string& file_path)
{
  return std::unique_ptr<reader>(new file_adapter(file_path.c_str(), file_mode::read));
}

std::unique_ptr<writer> open_compressed_file_writer(const std::string& file_path)
{
  return std::unique_ptr<writer>(new gzip_file_adapter(file_path.c_str(), file_mode::write));
}

std::unique_ptr<reader> open_compressed_file_reader(const std::string& file_path)
{
  return std::unique_ptr<reader>(new gzip_file_adapter(file_path.c_str(), file_mode::read));
}

std::unique_ptr<reader> open_compressed_stdin() { return std::unique_ptr<reader>(new gzip_stdio_adapter()); }

std::unique_ptr<writer> open_compressed_stdout() { return std::unique_ptr<writer>(new gzip_stdio_adapter()); }

std::unique_ptr<reader> open_stdin() { return std::unique_ptr<reader>(new stdio_adapter); }

std::unique_ptr<writer> open_stdout() { return std::unique_ptr<writer>(new stdio_adapter); }

std::unique_ptr<socket> wrap_socket_descriptor(int fd) { return std::unique_ptr<socket>(new socket(fd)); }

std::unique_ptr<writer> create_vector_writer(std::shared_ptr<std::vector<char>>& buffer)
{
  return std::unique_ptr<writer>(new vector_writer(buffer));
}

std::unique_ptr<reader> create_buffer_view(const char* data, size_t len)
{
  return std::unique_ptr<reader>(new buffer_view(data, len));
}
}  // namespace io
}  // namespace VW

//
// socket_adapter
//

ssize_t socket_adapter::read(char* buffer, size_t num_bytes)
{
#ifdef _WIN32
  return recv(_socket_fd, buffer, (int)(num_bytes), 0);
#else
  return ::read(_socket_fd, buffer, (unsigned int)num_bytes);
#endif
}

ssize_t socket_adapter::write(const char* buffer, size_t num_bytes)
{
#ifdef _WIN32
  return send(_socket_fd, buffer, (int)(num_bytes), 0);
#else
  return ::write(_socket_fd, buffer, (unsigned int)num_bytes);
#endif
}

details::socket_closer::socket_closer(int fd) : _socket_fd(fd) {}

details::socket_closer::~socket_closer()
{
#ifdef _WIN32
  closesocket(_socket_fd);
#else
  close(_socket_fd);
#endif
}

std::unique_ptr<reader> socket::get_reader()
{
  return std::unique_ptr<reader>(new socket_adapter(_socket_fd, _closer));
}

std::unique_ptr<writer> socket::get_writer()
{
  return std::unique_ptr<writer>(new socket_adapter(_socket_fd, _closer));
}

//
// stdio_adapter
//

ssize_t stdio_adapter::read(char* buffer, size_t num_bytes)
{
  std::cin.read(buffer, num_bytes);
  return std::cin.gcount();
}

ssize_t stdio_adapter::write(const char* buffer, size_t num_bytes)
{
  std::cout.write(buffer, num_bytes);
  // TODO is there a reliable way to do this?
  return num_bytes;
}

//
// file_adapter
//

file_adapter::file_adapter(const char* filename, file_mode mode) : reader(true /*is_resettable*/), _mode(mode)
{
#ifdef _WIN32
  if (_mode == file_mode::read)
  {
    // _O_SEQUENTIAL hints to OS that we'll be reading sequentially, so cache aggressively.
    _sopen_s(&_file_descriptor, filename, _O_RDONLY | _O_BINARY | _O_SEQUENTIAL, _SH_DENYWR, 0);
  }
  else
  {
    _sopen_s(
        &_file_descriptor, filename, _O_CREAT | _O_WRONLY | _O_BINARY | _O_TRUNC, _SH_DENYWR, _S_IREAD | _S_IWRITE);
  }
#else
  if (_mode == file_mode::read) { _file_descriptor = open(filename, O_RDONLY | O_LARGEFILE); }
  else
  {
    _file_descriptor = open(filename, O_CREAT | O_WRONLY | O_LARGEFILE | O_TRUNC, 0666);
  }
#endif

  if (_file_descriptor == -1 && *filename != '\0') { THROWERRNO("can't open: " << filename); }
}

file_adapter::file_adapter(int file_descriptor, file_mode mode)
    : reader(true /*is_resettable*/), _file_descriptor(file_descriptor), _mode(mode)
{
}

ssize_t file_adapter::read(char* buffer, size_t num_bytes)
{
  assert(_mode == file_mode::read);
#ifdef _WIN32
  return ::_read(_file_descriptor, buffer, (unsigned int)num_bytes);
#else
  return ::read(_file_descriptor, buffer, (unsigned int)num_bytes);
#endif
}

ssize_t file_adapter::write(const char* buffer, size_t num_bytes)
{
  assert(_mode == file_mode::write);
#ifdef _WIN32
  return ::_write(_file_descriptor, buffer, (unsigned int)num_bytes);
#else
  return ::write(_file_descriptor, buffer, (unsigned int)num_bytes);
#endif
}

void file_adapter::reset()
{
#ifdef _WIN32
  ::_lseek(_file_descriptor, 0, SEEK_SET);
#else
  ::lseek(_file_descriptor, 0, SEEK_SET);
#endif
}

file_adapter::~file_adapter()
{
#ifdef _WIN32
  ::_close(_file_descriptor);
#else
  ::close(_file_descriptor);
#endif
}

//
// gzip_file_adapter
//

gzip_file_adapter::gzip_file_adapter(const char* filename, file_mode mode) : reader(true /*is_resettable*/), _mode(mode)
{
  auto file_mode_arg = _mode == file_mode::read ? "rb" : "wb";
  _gz_file = gzopen(filename, file_mode_arg);
  // TODO test for failure
}

gzip_file_adapter::gzip_file_adapter(int file_descriptor, file_mode mode) : reader(true /*is_resettable*/), _mode(mode)
{
  auto file_mode_arg = _mode == file_mode::read ? "rb" : "wb";
  _gz_file = gzdopen(file_descriptor, file_mode_arg);
}

gzip_file_adapter::~gzip_file_adapter() { gzclose(_gz_file); }

ssize_t gzip_file_adapter::read(char* buffer, size_t num_bytes)
{
  assert(_mode == file_mode::read);

  auto num_read = gzread(_gz_file, buffer, (unsigned int)num_bytes);
  return (num_read > 0) ? (size_t)num_read : 0;
}

ssize_t gzip_file_adapter::write(const char* buffer, size_t num_bytes)
{
  assert(_mode == file_mode::write);

  auto num_written = gzwrite(_gz_file, buffer, (unsigned int)num_bytes);
  return (num_written > 0) ? (size_t)num_written : 0;
}

void gzip_file_adapter::reset() { gzseek(_gz_file, 0, SEEK_SET); }

//
// gzip_stdio_adapter
//

gzip_stdio_adapter::gzip_stdio_adapter() : reader(false /*is_resettable*/)
{
#ifdef _WIN32
  _gz_stdin = gzdopen(_fileno(stdin), "rb");
  _gz_stdout = gzdopen(_fileno(stdout), "wb");
#else
  _gz_stdin = gzdopen(fileno(stdin), "rb");
  _gz_stdout = gzdopen(fileno(stdout), "wb");
#endif
}

gzip_stdio_adapter::~gzip_stdio_adapter()
{
  gzclose(_gz_stdin);
  gzclose(_gz_stdout);
}

ssize_t gzip_stdio_adapter::read(char* buffer, size_t num_bytes)
{
  auto num_read = gzread(_gz_stdin, buffer, (unsigned int)num_bytes);
  return (num_read > 0) ? (size_t)num_read : 0;
}

ssize_t gzip_stdio_adapter::write(const char* buffer, size_t num_bytes)
{
  auto num_written = gzwrite(_gz_stdout, buffer, (unsigned int)num_bytes);
  return (num_written > 0) ? (size_t)num_written : 0;
}

//
// vector_writer
//

vector_writer::vector_writer(std::shared_ptr<std::vector<char>>& buffer) : _buffer(buffer) {}

ssize_t vector_writer::write(const char* buffer, size_t num_bytes)
{
  _buffer->reserve(_buffer->size() + num_bytes);
  _buffer->insert(std::end(*_buffer), buffer, buffer + num_bytes);
  return num_bytes;
}

//
// buffer_view
//

buffer_view::buffer_view(const char* data, size_t len) : reader(true), _data(data), _read_head(data), _len(len) {}

ssize_t buffer_view::read(char* buffer, size_t num_bytes)
{
  num_bytes = std::min((_data + _len) - _read_head, static_cast<std::ptrdiff_t>(num_bytes));
  if (num_bytes == 0) return 0;

  std::memcpy(buffer, _read_head, num_bytes);
  _read_head += num_bytes;

  return num_bytes;
}
void buffer_view::reset() { _read_head = _data; }
