#ifndef STREAM_HH
#define STREAM_HH 
#include <string>
#include <memory>
#include <iostream>
#include <fstream>

enum class CompressionFormat {
    NONE,
    GZIP,
    XZ
};

class InputStreamWrapper {
public:
    virtual ~InputStreamWrapper() = default;
    virtual bool read(char* buffer, std::streamsize size) = 0;
    virtual bool is_open() const = 0;
    virtual void close() = 0;
};

std::unique_ptr<InputStreamWrapper> create_input_stream(const std::string& filename);
#endif  