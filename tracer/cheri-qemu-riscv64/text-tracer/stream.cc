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

#include "stream.hh"
#include <cstdio>

CompressionFormat detect_compression_format(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        return CompressionFormat::NONE;
    }
    
    unsigned char magic[6] = {0};
    file.read(reinterpret_cast<char*>(magic), 6);
    file.close();
    
    if (magic[0] == 0x1f && magic[1] == 0x8b) {
        return CompressionFormat::GZIP;
    }
    
    if (magic[0] == 0xfd && magic[1] == 0x37 && magic[2] == 0x7a && 
        magic[3] == 0x58 && magic[4] == 0x5a && magic[5] == 0x00) {
        return CompressionFormat::XZ;
    }
    
    return CompressionFormat::NONE;
}

class UncompressedStream : public InputStreamWrapper {
private:
    std::ifstream file;
    
public:
    UncompressedStream(const std::string& filename) : file(filename) {}
    
    bool getline(std::string& line) override {
        return static_cast<bool>(std::getline(file, line));
    }
    
    bool good() const override {
        return file.good();
    }
    
    bool is_open() const override {
        return file.is_open();
    }
    
    void close() override {
        file.close();
    }
};

class GzipStream : public InputStreamWrapper {
private:
    FILE* pipe;
    
public:
    GzipStream(const std::string& filename) : pipe(nullptr) {
        std::string command = "gzip -dc \"" + filename + "\"";
        pipe = popen(command.c_str(), "r");
    }
    
    ~GzipStream() {
        if (pipe) {
            pclose(pipe);
        }
    }
    
    bool getline(std::string& line) override {
        if (!pipe) return false;
        
        char buffer[16384];
        if (fgets(buffer, sizeof(buffer), pipe)) {
            line = buffer;
            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
            }
            return true;
        }
        return false;
    }
    
    bool good() const override {
        return pipe != nullptr && !feof(pipe);
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

class XzStream : public InputStreamWrapper {
private:
    FILE* pipe;
    
public:
    XzStream(const std::string& filename) : pipe(nullptr) {
        std::string command = "xz -dc \"" + filename + "\"";
        pipe = popen(command.c_str(), "r");
    }
    
    ~XzStream() {
        if (pipe) {
            pclose(pipe);
        }
    }
    
    bool getline(std::string& line) override {
        if (!pipe) return false;
        
        char buffer[16384];
        if (fgets(buffer, sizeof(buffer), pipe)) {
            line = buffer;
            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
            }
            return true;
        }
        return false;
    }
    
    bool good() const override {
        return pipe != nullptr && !feof(pipe);
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

std::unique_ptr<InputStreamWrapper> create_input_stream(const std::string& filename) {
    CompressionFormat format = detect_compression_format(filename);
    
    switch (format) {
        case CompressionFormat::GZIP:
            return std::make_unique<GzipStream>(filename);
        case CompressionFormat::XZ:
            return std::make_unique<XzStream>(filename);
        case CompressionFormat::NONE:
        default:
            return std::make_unique<UncompressedStream>(filename);
    }
}