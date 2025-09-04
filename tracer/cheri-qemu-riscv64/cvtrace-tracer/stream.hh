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