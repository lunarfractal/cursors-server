#define ASIO_STANDALONE

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <iostream>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <random>

#define OPCODE_CS_PING 0x00
#define OPCODE_CS_PONG 0x01
#define OPCODE_DIMENSIONS 0x02
#define OPCODE_ENTER_GAME 0x03
#define OPCODE_CURSOR 0x04

#define OPCODE_SC_PING 0x00
#define OPCODE_SC_PONG 0x01
#define OPCODE_ENTERED_GAME 0xA1
#define OPCODE_CURSOR_INFO 0xA2

#define OPCODE_CURSOR_DELETE 0xA3

using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

typedef websocketpp::server<websocketpp::config::asio> server;
typedef websocketpp::connection_hdl connection_hdl;
typedef server::message_ptr message_ptr;


uint16_t getUniqueId() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()
              ).count();
    uint32_t timeComponent = static_cast<uint32_t>(ms);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dis(0, 0xFFFF);
    uint16_t randomComponent = dis(gen);
    uint16_t uniqueId = static_cast<uint16_t>((timeComponent ^ randomComponent) & 0xFFFF);
    return uniqueId;
}

class IO {
public:
    IO() : sentHello(false), sentPing(false), inGame(false), screen_width(0), screen_height(0), x(0), y(0) {}
    bool sentHello, sentPing, inGame;
    uint16_t screen_width, screen_height, x, y;
    uint32_t id;

    bool didSendHello() {
       return sentPing && sentHello;
    }
private:
};


class WebSocketServer {
    public:
        WebSocketServer() {
            m_server.init_asio();

            m_server.set_open_handler(bind(&WebSocketServer::on_open,this,::_1));
            m_server.set_close_handler(bind(&WebSocketServer::on_close,this,::_1));
            m_server.set_message_handler(bind(&WebSocketServer::on_message,this,::_1,::_2));

            m_server.clear_access_channels(websocketpp::log::alevel::all);

            std::thread([this]() {
                while(true) {
                    sendInfo();
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                }
            }).detach();
        }

        void ping(connection_hdl hdl) {
            uint8_t buffer[] = {OPCODE_SC_PING};
            try {
                m_server.send(hdl, buffer, sizeof(buffer), websocketpp::frame::opcode::binary);
            } catch (websocketpp::exception const & e) {
                std::cout << "Ping failed because: "
                    << "(" << e.what() << ")" << std::endl;
            }
        }

        void pong(connection_hdl hdl) {
            uint8_t buffer[] = {OPCODE_SC_PONG};
            try {
                m_server.send(hdl, buffer, sizeof(buffer), websocketpp::frame::opcode::binary);
            } catch (websocketpp::exception const & e) {
                std::cout << "Pong failed because: "
                    << "(" << e.what() << ")" << std::endl;
            }
        }

        void sendId(connection_hdl hdl, uint16_t id) {
            uint8_t buffer[5];
            buffer[0] = OPCODE_ENTERED_GAME;
            std::memcpy(&buffer[1], &id, sizeof(uint16_t));
            try {
                m_server.send(hdl, buffer, sizeof(buffer), websocketpp::frame::opcode::binary);
            } catch (websocketpp::exception const & e) {
                std::cout << "Send failed because: "
                    << "(" << e.what() << ")" << std::endl;
            }
        }

        void sendAll(std::vector<uint8_t> &buffer) {
            for (auto &pair: m_connections) {
                try {
                    if (
                        m_server.get_con_from_hdl(pair.first)->get_state() == websocketpp::session::state::open
                        && pair.second.didSendHello() && pair.second.inGame
                    ) {
                        m_server.send(pair.first, buffer.data(), buffer.size(), websocketpp::frame::opcode::binary);
                    }
                } catch (websocketpp::exception const & e) {
                    std::cout << "Send failed because: "
                        << "(" << e.what() << ")" << std::endl;
                }
            }
        }

        void processMessage(std::vector<uint8_t> &buffer, connection_hdl hdl) {
            IO &io = m_connections[hdl];
            uint8_t op = buffer[0];
            switch(op) {
                case OPCODE_CS_PING:
                {
                    pong(hdl);
                    if(!io.sentPing) io.sentPing = true;
                    break;
                }
                case OPCODE_CS_PONG:
                {
                    std::cout << "Pong!" << std::endl;
                    break;
                }
                case OPCODE_DIMENSIONS:
                {
                    if(buffer.size() >= 5) {
                        std::memcpy(&io.screen_width, &buffer[1], sizeof(uint16_t));
                        std::memcpy(&io.screen_height, &buffer[3], sizeof(uint16_t));
                    }
                    if(!io.sentHello) io.sentHello = true;
                    break;
                }
                case OPCODE_ENTER_GAME:
                {
                    if(!io.inGame && io.didSendHello()) {
                        uint16_t id = getUniqueId();
                        io.inGame = true;
                        io.id = id;
                        sendId(hdl, id);
                    }
                    break;
                }
                case OPCODE_CURSOR:
                {
                    if(io.inGame && buffer.size() >= 5) {
                        uint16_t x, y;
                        std::memcpy(&x, &buffer[1], 2);
                        std::memcpy(&y, &buffer[3], 2);
                        x = (x * 65535) / io.screen_width;
                        y = (y * 65535) / io.screen_height;
                        io.x = x; io.y = y;
                    }
                    break;
                }
                default:
                {
                    std::cout << "unknown op " << (int)op << std::endl;
                    break;
                }
            }
        }

        void sendBuffer(connection_hdl hdl, std::vector<uint8_t> &buffer) {
            try {
                m_server.send(hdl, buffer.data(), buffer.size(), websocketpp::frame::opcode::binary);
            } catch (websocketpp::exception const & e) {
                std::cout << "Send failed because: "
                    << "(" << e.what() << ")" << std::endl;
            }
        }

        void sendInfo() {
            std::vector<uint8_t> buffer(1 + 7 * m_connections.size());
            buffer[0] = OPCODE_CURSOR_INFO;
            int offset = 1;
            for(auto &pair: m_connections) {
                if(pair.second.inGame) {
                    std::memcpy(&buffer[offset], &pair.second.id, 2);
                    offset += 2;
                    std::memcpy(&buffer[offset], &pair.second.x, 2);
                    offset += 2;
                    std::memcpy(&buffer[offset], &pair.second.y, 2);
                    offset += 2;
                }
            }
            buffer.insert(buffer.end(), {0x00, 0x00});
            sendAll(buffer);
        }

        void on_open(connection_hdl hdl) {
            IO io;
            m_connections[hdl] = io;
        }

        void on_close(connection_hdl hdl) {
            IO &io = m_connections[hdl];
            io.inGame = false;
            std::vector<uint8_t> buffer(1+2);
            buffer[0] = OPCODE_CURSOR_DELETE;
            std::memcpy(&buffer[1], &io.id, 2);
            sendAll(buffer);
            m_connections.erase(hdl);
        }

        void on_message(connection_hdl hdl, server::message_ptr msg) {
            if (msg->get_opcode() == websocketpp::frame::opcode::binary) {
                std::vector<uint8_t> buffer(msg->get_payload().begin(), msg->get_payload().end());
                processMessage(buffer, hdl);
            } else {
                std::cout << "received text message from hdl: " << hdl.lock().get()
                    << " and message: " << msg->get_payload() << std::endl;
            }
        }

        void run(uint16_t port) {
            m_server.listen(port);
            m_server.start_accept();
            m_server.run();
        }
    private:
        server m_server;
        typedef struct {
            std::size_t operator()(const websocketpp::connection_hdl& hdl) const {
                return std::hash<std::uintptr_t>()(reinterpret_cast<std::uintptr_t>(hdl.lock().get()));
            }
        } connection_hdl_hash;
        typedef struct {
            bool operator()(const websocketpp::connection_hdl& lhs, const websocketpp::connection_hdl& rhs) const {
                return !lhs.owner_before(rhs) && !rhs.owner_before(lhs);
            }
        } connection_hdl_equal;
        std::unordered_map<connection_hdl, IO, connection_hdl_hash, connection_hdl_equal> m_connections;
};

int main() {
   WebSocketServer wsServer;
   wsServer.run(8081);
}
