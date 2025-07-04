#include "stream.hh"

// Function to detect compression format by reading magic bytes
CompressionFormat detect_compression_format(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        return CompressionFormat::NONE;
    }
    
    unsigned char magic[6] = {0};
    file.read(reinterpret_cast<char*>(magic), 6);
    file.close();
    
    // Check for gzip magic bytes (1f 8b)
    if (magic[0] == 0x1f && magic[1] == 0x8b) {
        return CompressionFormat::GZIP;
    }
    
    // Check for xz magic bytes (fd 37 7a 58 5a 00)
    if (magic[0] == 0xfd && magic[1] == 0x37 && magic[2] == 0x7a && 
        magic[3] == 0x58 && magic[4] == 0x5a && magic[5] == 0x00) {
        return CompressionFormat::XZ;
    }
    
    return CompressionFormat::NONE;
}

// Implementation for uncompressed files
class UncompressedStream : public InputStreamWrapper {
private:
    std::ifstream file;
    
public:
    UncompressedStream(const std::string& filename) : file(filename, std::ios::binary) {}
    
    bool read(char* buffer, std::streamsize size) override {
        return static_cast<bool>(file.read(buffer, size));
    }
    
    bool is_open() const override {
        return file.is_open();
    }
    void close() override {
        file.close();
    }
};

// Implementation for gzip compressed files
class GzipStream : public InputStreamWrapper {
private:
    FILE* pipe;
    std::string command;
    
public:
    GzipStream(const std::string& filename) : pipe(nullptr) {
        // Use zcat or gzip -dc to decompress on the fly
        command = "gzip -dc \"" + filename + "\"";
        pipe = popen(command.c_str(), "r");
    }
    
    ~GzipStream() {
        if (pipe) {
            pclose(pipe);
        }
    }
    
    bool read(char* buffer, std::streamsize size) override {
        if (!pipe) return false;
        size_t read_bytes = fread(buffer, 1, size, pipe);
        return read_bytes == static_cast<size_t>(size);
    }
    
    bool is_open() const override {
        return pipe != nullptr;
    }
    
    void close() override {
        if (pipe) {
            pclose(pipe);
            pipe = nullptr;
        }
    }
};

// Implementation for xz compressed files
class XzStream : public InputStreamWrapper {
private:
    FILE* pipe;
    std::string command;
    
public:
    XzStream(const std::string& filename) : pipe(nullptr) {
        // Use xzcat or xz -dc to decompress on the fly
        command = "xz -dc \"" + filename + "\"";
        pipe = popen(command.c_str(), "r");
    }
    
    ~XzStream() {
        if (pipe) {
            pclose(pipe);
        }
    }
    
    bool read(char* buffer, std::streamsize size) override {
        if (!pipe) return false;
        size_t read_bytes = fread(buffer, 1, size, pipe);
        return read_bytes == static_cast<size_t>(size);
    }
    
    bool is_open() const override {
        return pipe != nullptr;
    }
    
    void close() override {
        if (pipe) {
            pclose(pipe);
            pipe = nullptr;
        }
    }
};

// Factory function to create appropriate stream wrapper
std::unique_ptr<InputStreamWrapper> create_input_stream(const std::string& filename) {
    CompressionFormat format = detect_compression_format(filename);
    
    switch (format) {
        case CompressionFormat::GZIP:
            std::cerr << "Detected gzip compression for " << filename << std::endl;
            return std::make_unique<GzipStream>(filename);
            
        case CompressionFormat::XZ:
            std::cerr << "Detected xz compression for " << filename << std::endl;
            return std::make_unique<XzStream>(filename);
            
        case CompressionFormat::NONE:
        default:
            std::cerr << "No compression detected for " << filename << std::endl;
            return std::make_unique<UncompressedStream>(filename);
    }
}