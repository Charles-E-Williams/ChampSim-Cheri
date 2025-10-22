/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tracereader.hh"

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
class UncompressedTrace : public TraceReaderWrapper {
private:
    std::ifstream file;
    
public:
    UncompressedTrace(const std::string& filename) : file(filename, std::ios::binary) {}
    
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
class GzipTrace : public TraceReaderWrapper {
private:
    FILE* pipe;
    std::string command;
    
public:
    GzipTrace(const std::string& filename) : pipe(nullptr) {
        // Use zcat or gzip -dc to decompress on the fly
        command = "gzip -dc \"" + filename + "\"";
        pipe = popen(command.c_str(), "r");
    }
    
    ~GzipTrace() {
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
class XzTrace : public TraceReaderWrapper {
private:
    FILE* pipe;
    std::string command;
    
public:
    XzTrace(const std::string& filename) : pipe(nullptr) {
        // Use xzcat or xz -dc to decompress on the fly
        command = "xz -dc \"" + filename + "\"";
        pipe = popen(command.c_str(), "r");
    }
    
    ~XzTrace() {
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

// Factory function to create appropriate tracereader wrapper
std::unique_ptr<TraceReaderWrapper> create_input_stream(const std::string& filename) {
    CompressionFormat format = detect_compression_format(filename);
    
    switch (format) {
        case CompressionFormat::GZIP:
            return std::make_unique<GzipTrace>(filename);
            
        case CompressionFormat::XZ:
            return std::make_unique<XzTrace>(filename);
            
        case CompressionFormat::NONE:
        default:
            return std::make_unique<UncompressedTrace>(filename);
    }
}