
#include "simple_socket/modbus/ModbusServer.hpp"

#include "simple_socket/TCPSocket.hpp"
#include "simple_socket/modbus/HoldingRegister.hpp"

#include <array>
#include <iostream>
#include <thread>

using namespace simple_socket;

namespace {

    // Send exception response
    void sendException(SimpleConnection& connection, uint8_t deviceAddress, uint8_t functionCode, uint8_t exceptionCode) {
        std::array<uint8_t, 3> response{};
        response[0] = deviceAddress;
        response[1] = functionCode | 0x80;// Set error flag
        response[2] = exceptionCode;

        connection.write(response);
    }

    void processRequest(SimpleConnection& conn, const std::vector<uint8_t>& request, HoldingRegister& reg) {
        const int8_t headerSize = 6;
        const uint8_t functionCode = request[headerSize + 1];
        const uint16_t startAddress = (request[headerSize + 2] << 8) | request[headerSize + 3];
        const uint16_t quantity = (request[headerSize + 4] << 8) | request[headerSize + 5];

        switch (functionCode) {
            case 0x03: {// Read Holding Registers
                if (startAddress + quantity > reg.size()) {
                    sendException(conn, request[headerSize + 0], functionCode, 0x02);// Illegal Data Address
                    return;
                }

                // Prepare response: MBAP header + PDU
                std::vector response(request.begin(), request.begin() + 4);
                const uint16_t length = 3 + (quantity * 2);// Length of PDU
                response.push_back(length >> 8);           // Length (High)
                response.push_back(length & 0xFF);         // Length (Low)
                response.push_back(request[6]);            // Unit Identifier

                // PDU
                response.push_back(functionCode);// Function Code
                response.push_back(quantity * 2);// Byte Count
                // Append register values to the response
                for (uint16_t i = 0; i < quantity; ++i) {
                    const uint16_t regValue = reg.getUint16(startAddress + i);
                    response.push_back(regValue >> 8);
                    response.push_back(regValue & 0xFF);
                }

                conn.write(response);
                break;
            }

            case 0x06: {// Write Single Register
                if (startAddress >= reg.size()) {
                    sendException(conn, request[headerSize + 0], functionCode, 0x02);// Illegal Data Address
                    return;
                }

                const uint16_t valueToWrite = (request[headerSize + 4] << 8) | request[headerSize + 5];
                reg.setUint16(startAddress, valueToWrite);

                // Echo back the same request as a confirmation
                const std::vector response(request.begin(), request.end());
                conn.write(response);
                break;
            }

            case 0x10: {// Write Multiple Registers
                // Extract start address and quantity of registers from the request
                const uint16_t startAddress = (request[headerSize + 2] << 8) | request[headerSize + 3];
                const uint16_t quantity = (request[headerSize + 4] << 8) | request[headerSize + 5];
                const uint8_t byteCount = request[headerSize + 6];// Number of data bytes

                // Check if the request is valid: the quantity of registers should not exceed the register size.
                if (startAddress + quantity > reg.size() || byteCount != quantity * 2) {
                    sendException(conn, request[headerSize + 0], functionCode, 0x02);// Illegal Data Address
                    return;
                }

                // Write data to the registers
                for (uint16_t i = 0; i < quantity; ++i) {
                    uint16_t regValue = (request[headerSize + 7 + (i * 2)] << 8) | request[headerSize + 8 + (i * 2)];
                    reg.setUint16(startAddress + i, regValue);// Write the value to the register
                }

                // Prepare response (Echo start address and quantity of registers written)
                std::vector response(request.begin(), request.begin() + 6);// Copy MBAP header
                response.push_back(request[headerSize + 1]);               // Unit Identifier
                response.push_back(functionCode);                          // Function Code
                response.push_back(startAddress >> 8);                     // Starting Address (High byte)
                response.push_back(startAddress & 0xFF);                   // Starting Address (Low byte)
                response.push_back(quantity >> 8);                         // Quantity of Registers (High byte)
                response.push_back(quantity & 0xFF);                       // Quantity of Registers (Low byte)

                conn.write(response);
                break;
            }

            default:
                sendException(conn, request[0], functionCode, 0x01);// Illegal Function
                break;
        }
    }

}// namespace

struct ModbusServer::Impl {

    Impl(uint16_t port, HoldingRegister& reg)
        : server_(port), register_(&reg) {}

    void start() {
        thread_ = std::thread([this] {
            try {
                while (!stop_) {
                    auto conn = server_.accept();
                    clients_.emplace_back(&Impl::clientThread, this, std::move(conn));
                }
            } catch (const std::exception&) {}
        });
    }

    void stop() {
        stop_ = true;
        server_.close();
    }

    void clientThread(std::unique_ptr<SimpleConnection> conn) {

        std::array<uint8_t, 6> mbap{};
        std::vector<uint8_t> request;
        while (!stop_) {

            if (!conn->readExact(mbap)) {
                std::cerr << "Error reading request or connection closed\n";
                break;// Exit the loop if there’s an error or the connection is closed
            }

            // Extract the length from the MBAP header (bytes 4 and 5)
            // Length field specifies the number of bytes following the Unit Identifier
            const uint16_t length = (mbap[4] << 8) | mbap[5];

            // The total frame length is the 7 bytes MBAP header + the length in MBAP (following Unit Identifier)
            request.resize(length);// Resize request to fit the full frame (MBAP + Modbus request)
            conn->readExact(request);

            request.insert(request.begin(), mbap.begin(), mbap.end());
            processRequest(*conn, request, *register_);
        }
    }

    ~Impl() {
        for (auto& client : clients_) {
            if (client.joinable()) client.join();
        }
        if (thread_.joinable()) thread_.join();
    }

private:
    TCPServer server_;
    HoldingRegister* register_;

    std::atomic_bool stop_;
    std::thread thread_;
    std::vector<std::thread> clients_;
};

ModbusServer::ModbusServer(HoldingRegister& reg, uint16_t port)
    : pimpl_(std::make_unique<Impl>(port, reg)) {}

void ModbusServer::start() {
    pimpl_->start();
}

void ModbusServer::stop() {
    pimpl_->stop();
}

ModbusServer::~ModbusServer() = default;
