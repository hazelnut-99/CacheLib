#pragma once

#include <folly/dynamic.h>
#include <folly/json.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <folly/logging/xlog.h>

class ModelApiClient {
public:
    explicit ModelApiClient(const std::string& baseUrl) : baseUrl_(baseUrl) {}

    // Create a model
    void createModel(const std::string& modelName, const std::string& modelType) {
        std::string path = "/api/create_model";
        folly::dynamic payload = folly::dynamic::object("model_name", modelName)("model_type", modelType);
        sendPostRequest(path, folly::toJson(payload));
    }

    // Create a detector
    void createDetector(const std::string& modelName) {
        std::string path = "/api/create_detector";
        folly::dynamic payload = folly::dynamic::object("model_name", modelName);
        sendPostRequest(path, folly::toJson(payload));
    }

    // Fit the model
    void fitModel(const std::string& modelName, int x1, int x2, int y) {
        std::string path = "/api/fit";
        folly::dynamic payload = folly::dynamic::object("model_name", modelName)("x1", x1)("x2", x2)("y", y);
        sendPostRequest(path, folly::toJson(payload));
    }

    // Predict using the model
    int predict(const std::string& modelName, int x1, int x2) {
        std::string path = "/api/predict";
        folly::dynamic payload = folly::dynamic::object("model_name", modelName)("x1", x1)("x2", x2);
        std::string response = sendPostRequest(path, folly::toJson(payload));

        // Parse the JSON response
        try {
            folly::dynamic jsonResponse = folly::parseJson(response);
            if (jsonResponse.isObject() && jsonResponse.count("predictions")) {
                return jsonResponse["predictions"].asInt();
            } else {
                throw std::runtime_error("Response does not contain 'predictions'");
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing JSON response: " << e.what() << std::endl;
            throw;
        }
    }

    // Predict using the model
    int detect(const std::string& modelName, double x) {
        std::string path = "/api/detect";
        folly::dynamic payload = folly::dynamic::object("model_name", modelName)("x", x);
        std::string response = sendPostRequest(path, folly::toJson(payload));

        // Parse the JSON response
        try {
            folly::dynamic jsonResponse = folly::parseJson(response);
            if (jsonResponse.isObject() && jsonResponse.count("predictions")) {
                return jsonResponse["predictions"].asInt();
            } else {
                throw std::runtime_error("Response does not contain 'predictions'");
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing JSON response: " << e.what() << std::endl;
            throw;
        }
    }

private:
    std::string baseUrl_;

    // Helper function to handle HTTP POST requests
    std::string sendPostRequest(const std::string& path, const std::string& payload) {
        const int bufferSize = 4096;
        char buffer[bufferSize];
    
        // Extract the host and port from the base URL
        std::string host = extractHost(baseUrl_);
        int port = extractPort(baseUrl_);
    
        // Create a socket
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            throw std::runtime_error("Failed to create socket");
        }
    
        // Resolve the host
        struct hostent* server = gethostbyname(host.c_str());
        if (!server) {
            close(sock);
            throw std::runtime_error("Failed to resolve host");
        }
    
        // Set up the server address structure
        struct sockaddr_in serverAddr;
        std::memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        std::memcpy(&serverAddr.sin_addr.s_addr, server->h_addr, server->h_length);
        serverAddr.sin_port = htons(port);
    
        // Connect to the server
        if (connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
            close(sock);
            throw std::runtime_error("Failed to connect to server");
        }
    
        // Create the HTTP POST request
        std::string request = "POST " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
        request += "Connection: close\r\n\r\n";
        request += payload;
    
        // Send the request
        if (send(sock, request.c_str(), request.size(), 0) < 0) {
            close(sock);
            throw std::runtime_error("Failed to send request");
        }
    
        // Receive the response
        std::string response;
        int bytesRead;
        while ((bytesRead = recv(sock, buffer, bufferSize - 1, 0)) > 0) {
            buffer[bytesRead] = '\0';
            response += buffer;
        }
    
        if (bytesRead < 0) {
            close(sock);
            throw std::runtime_error("Failed to read response");
        }
    
        // Close the socket
        close(sock);
    
        // Extract the body from the HTTP response
        size_t bodyPos = response.find("\r\n\r\n");
        if (bodyPos != std::string::npos) {
            return response.substr(bodyPos + 4);
        } else {
            throw std::runtime_error("Invalid HTTP response");
        }
    }

    std::string extractHost(const std::string& url) {
        size_t start = url.find("://");
        if (start == std::string::npos) {
            start = 0; // No protocol, start from the beginning
        } else {
            start += 3; // Skip "://"
        }
    
        size_t colonPos = url.find(":", start); // Look for the colon indicating the port
        size_t slashPos = url.find("/", start); // Look for the slash indicating the path
    
        if (colonPos != std::string::npos && (slashPos == std::string::npos || colonPos < slashPos)) {
            // If a colon is found before a slash, extract up to the colon (exclude the port)
            return url.substr(start, colonPos - start);
        }
    
        // Otherwise, extract up to the slash or end of the string
        return url.substr(start, slashPos - start);
    }


    int extractPort(const std::string& url) {
        size_t start = url.find("://");
        if (start == std::string::npos) {
            start = 0;
        } else {
            start += 3;
        }
    
        size_t colonPos = url.find(":", start);
        size_t slashPos = url.find("/", start);
    
        if (colonPos != std::string::npos && (slashPos == std::string::npos || colonPos < slashPos)) {
            // Port is specified in the URL
            return std::stoi(url.substr(colonPos + 1, slashPos - colonPos - 1));
        }
    
        // Default to port 80 if no port is specified
        return 80;
    }
};