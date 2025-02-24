#pragma once

#include <fstream>
#include <vector>
#include <zstd.h>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <folly/logging/xlog.h>

typedef enum { ERR, OK, MY_EOF } rstatus;

enum ReadDirection {
  READ_FORWARD = 0,
  READ_BACKWARD = 1,
};

class ZstdReader {
public:
    ZstdReader() : zds(nullptr), bufferOutReadPos(0), status(OK), ignoreSizeZeroReq(1), readDirection(READ_FORWARD) {}

    ~ZstdReader() {
        closeReader();
    }

    bool openReader(const std::string& traceFileName) {
        closeReader(); // Ensure any previously opened file is closed

        inputFile.open(traceFileName, std::ios::binary);
        if (!inputFile.is_open()) {
            XLOGF(ERR, "Cannot open file: {}", traceFileName);
            return false;
        }

        bufferInSize = ZSTD_DStreamInSize();
        bufferIn.resize(bufferInSize);
        if (bufferIn.empty()) {
            XLOGF(ERR, "Failed to allocate memory for bufferIn");
            return false;
        }

        bufferOutSize = ZSTD_DStreamOutSize() * 2;
        bufferOut.resize(bufferOutSize);
        if (bufferOut.empty()) {
            XLOGF(ERR, "Failed to allocate memory for bufferOut");
            return false;
        }

        input.src = bufferIn.data();
        input.size = 0;
        input.pos = 0;

        output.dst = bufferOut.data();
        output.size = bufferOutSize;
        output.pos = 0;

        zds = ZSTD_createDStream();
        if (!zds) {
            XLOGF(ERR, "Failed to create ZSTD_DStream");
            return false;
        }

        size_t initResult = ZSTD_initDStream(zds);
        if (ZSTD_isError(initResult)) {
            XLOGF(ERR, "ZSTD_initDStream error: {}", ZSTD_getErrorName(initResult));
            ZSTD_freeDStream(zds);
            zds = nullptr;
            return false;
        }

        bufferOutReadPos = 0;
        status = OK;
        return true;
    }

    void closeReader() {
        if (zds) {
            ZSTD_freeDStream(zds);
            zds = nullptr;
        }
        if (inputFile.is_open()) {
            inputFile.close();
        }
    }

    void clear() {
        inputFile.clear(); // Clear the file stream state
        bufferIn.clear();
        bufferOut.clear();
        bufferOutReadPos = 0;
        status = OK;
        input.size = 0;
        input.pos = 0;
        output.pos = 0;
    }

    bool is_open() const {
        return inputFile.is_open() && zds != nullptr;
    }

    bool readBytesZstd(size_t size, char** start) {
        while (bufferOutReadPos + size > output.pos) {
            rstatus status = decompressFromBuffer();
            if (status != OK) {
                if (status != MY_EOF) {
                    XLOGF(ERR, "Error decompressing file");
                    return false;
                } else {
                    return false;
                }
            }
        }
        if (bufferOutReadPos + size <= output.pos) {
            *start = bufferOut.data() + bufferOutReadPos;
            bufferOutReadPos += size;
            return true;
        } else {
            XLOGF(ERR, "Not enough bytes available");
            return false;
        }
    }

    int ignoreSizeZeroReq;
    int readDirection;

private:
    std::ifstream inputFile;
    ZSTD_DStream *zds;
  
    size_t bufferInSize;
    std::vector<char> bufferIn;
    size_t bufferOutSize;
    std::vector<char> bufferOut;
  
    size_t bufferOutReadPos;
  
    ZSTD_inBuffer input;
    ZSTD_outBuffer output;
  
    rstatus status;

    size_t readFromFile() {
        inputFile.read(bufferIn.data(), bufferInSize);
        size_t readSize = inputFile.gcount();
        if (readSize < bufferInSize) {
            if (inputFile.eof()) {
                status = MY_EOF;
            } else {
                status = ERR;
                XLOGF(ERR, "Error reading from file");
                return 0;
            }
        }
        input.size = readSize;
        input.pos = 0;
        return readSize;
    }

    rstatus decompressFromBuffer() {
        void *bufferStart = bufferOut.data() + bufferOutReadPos;
        size_t bufferLeftSize = output.pos - bufferOutReadPos;
        memmove(bufferOut.data(), bufferStart, bufferLeftSize);
        output.pos = bufferLeftSize;
        bufferOutReadPos = 0;
  
        if (input.pos >= input.size) {
            size_t readSize = readFromFile();
            if (readSize == 0) {
                if (status == MY_EOF) {
                    return MY_EOF;
                } else {
                    XLOGF(ERR, "Error reading from file");
                    return ERR;
                }
            }
        }
  
        size_t const ret = ZSTD_decompressStream(zds, &output, &input);
        if (ret != 0) {
            if (ZSTD_isError(ret)) {
                XLOGF(ERR, "ZSTD decompression error: {}", ZSTD_getErrorName(ret));
                return ERR;
            }
        }
        return OK;
    }
};