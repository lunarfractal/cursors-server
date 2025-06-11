#define ASIO_STANDALONE

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <unordered_map>
#include <unordered_set>
#include <csignal>
#include <vector>

// A structure representing a cursor on the screen.
struct cursor_data {
    cursor_data(uint32_t i, std::string w) : id(i), x(0), y(0),
        nick(""), world(w) {}

    uint32_t id;

    // the cursor's position on it's own screen (pageX, pageY)
    uint32_t x, y;

    // color support?
    uint8_t redgreenblue[3];
    // nickname support?
    std::string nick;

    std::string world;

    uint8_t del; // whether its being deleted
    uint8_t is_full;

    std::unordered_set<uint32_t> view;

    // we need screen data to calculate the position relative to the other cursor's screen
    uint32_t screen_width = 1370, screen_height = 600; // the page's scrollable area (documentElement.scrollWidth, documentElement.scrollHeight)

    void get_data(std::vector<uint8_t>& buffer, size_t& offset, const cursor_data& other_cursor) {
        buffer.resize(offset + 4);
        
        std::memcpy(&buffer[offset], &id, 4);
        offset += 4;

        uint8_t flags = 0x1;
        
        if(other_cursor.view.find(id) == other_cursor.view.end()) {
            if(del) {
                buffer.resize(offset + 1);
                buffer[offset++] = 0x3;
                view.erase(other_cursor.id);
            } else {
                buffer.resize(offset + 9);
            
                flags = 0x0;
                other_cursor.view.insert(id); // my id

                buffer[offset++] = flags;

                float other_x = x * other_cursor.screen_width / 4294967295;
                float other_y = y * other_cursor.screen_height / 4294967295;
        
                std::memcpy(&buffer[offset], &other_x, 4);
                offset += 4;

                std::memcpy(&buffer[offset], &other_y, 4);
                offset += 4;
            }
        } else {
            buffer.resize(offset + 9 + nick.length() + 1 + 3);
            if(is_full) {
                flags = 0x2;
                is_full = 0x0;
            }
            
            buffer[offset++] = flags;
        
            float other_x = x * other_cursor.screen_width / 4294967295; // Ultra precision
            float other_y = y * other_cursor.screen_height / 4294967295;
        
            std::memcpy(&buffer[offset], &other_x, 4);
            offset += 4;

            std::memcpy(&buffer[offset], &other_y, 4);
            offset += 4;

            std::memcpy(&buffer[offset], nick.c_str(), nick.length() + 1); // +1 to include the null terminator
            offset += nick.length() + 1;

            std::memcpy(&buffer[offset], redgreenblue, 3);
            offset += 3;        
        }
    }
};

// util functions must follow camelCase in my opinion
uint32_t getUniqueId() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()
              ).count();

    uint32_t timeComponent = static_cast<uint32_t>(ms);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
    uint32_t randomComponent = dis(gen);

    uint32_t uniqueId = timeComponent ^ randomComponent;

    return uniqueId;
}


// now get to the webSocket server
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::condition_variable;

typedef websocketpp::server<websocketpp::config::asio> server;
typedef websocketpp::connection_hdl connection_hdl;
typedef server::message_ptr message_ptr;

class cursors_server {
public:
    cursors_server() {
        m_server.init_asio();

        m_server.set_open_handler(bind(&cursors_server::on_open,this,::_1));
        m_server.set_close_handler(bind(&cursors_server::on_close,this,::_1));
        m_server.set_message_handler(bind(&cursors_server::on_message,this,::_1,::_2));

        m_server.clear_access_channels(websocketpp::log::alevel::all); // I wanted to keep error logging
    }

    void run(uint16_t port) {
        m_server.listen(port);
        m_server.start_accept();

        m_server.get_alog().write(websocketpp::log::alevel::app, "Server listening on port " + std::to_string(port));

        try {
            m_server.run();
        } catch(const std::exception &e) {
            std::cout << e.what() << std::endl;
        }
    }

    void shutdown() {
        m_connections.clear();
        m_server.stop_listening();
    }

    void on_open(connection_hdl hdl) {
        constexpr uint8_t opcode_entered_game = 0xA1;
        
        server::connection_ptr con = m_server.get_con_from_hdl(hdl);
        std::string resource = con->get_resource();

        uint32_t id = getUniqueId();
        
        m_connections[hdl] = cursor_data(id, resource);

        uint8_t buffer[5];
        buffer[0] = opcode_entered_game;

        std::memcpy(&buffer[1], &id, 4);
        m_server.send(hdl, buffer, 5, websocketpp::frame::opcode::binary);

        m_action_cond.notify_one();
    }

    void on_close(connection_hdl hdl) {
        m_connections[hdl].del = 0x1;
        dels.insert(hdl);
    }

    void on_message(connection_hdl hdl, message_ptr msg) {
        if (msg->get_opcode() == websocketpp::frame::opcode::binary) {
            std::string payload = msg->get_payload();
            process_message(payload, hdl);
        } else if (msg->get_opcode() == websocketpp::frame::opcode::text) {} else {}
    }

    void process_message(std::string& buffer, connection_hdl hdl) {
        cursor_data& cursor = m_connections[hdl];

        constexpr uint8_t opcode_ping = 0x00;
        constexpr uint8_t opcode_hello = 0x01;
        constexpr uint8_t opcode_cursor = 0x02;
        constexpr uint8_t opcode_nick = 0x03;
        constexpr uint8_t opcode_resize = 0x04;
        constexpr uint8_t opcode_redgreenblue = 0x05;

        switch(buffer[0]) {
            case opcode_ping:
            {
                m_server.send(hdl, buffer, websocketpp::frame::opcode::binary);
                break;
            }

            case opcode_hello:
            {
                if(buffer.size() >= 9) {
                    std::memcpy(&cursor.screen_width, &buffer[1], 4);
                    std::memcpy(&cursor.screen_height, &buffer[5], 4);
                }

                break;
            }

            case opcode_resize:
            {
                if(buffer.size() >= 9) {
                    std::memcpy(&cursor.screen_width, &buffer[1], 4);
                    std::memcpy(&cursor.screen_height, &buffer[5], 4);
                }

                break;
            }

            case opcode_cursor:
            {
                if(buffer.size() >= 9) {
                    std::memcpy(&cursor.x, &buffer[1], 4);
                    std::memcpy(&cursor.y, &buffer[5], 4);
                }

                break;
            }

            case opcode_nick:
            {
                if(buffer.size() >= 1 + 3 + 3) {
                    cursor.nick.resize(buffer.size());
                    std::memcpy(cursor.nick.data(), &buffer[1], buffer.size());
                    cursor.is_full = 0x1;
                }

                break;
            }

            case opcode_redgreenblue:
            {
                if(buffer.size() > 4) {
                    std::memcpy(cursor.redgreenblue, buffer.data() + 1, 3);
                    cursor.is_full = 0x1;
                }
                
                break;
            }
        }
    }

    void cursor_loop() {
        constexpr uint8_t opcode_cursors_v1 = 0xA4;
        while(1) {
            while(m_actions.size() < 2) {
               m_cursors_cond.wait(lock);
            }
            
            for(auto &pair: m_connections) {
                std::vector<uint8_t> buffer(1);
                buffer[0] = opcode_cursors_v1;
                buffer.reserve(m_connections.size() * 30);
                int offset = 1;
            
                cursor_data& me = pair.second;
            
                for(auto &other_pair: m_connections) {
                    cursor_data& other = other_pair.second;

                    if(other.id == me.id) continue;
                    if(other.world != me.world) continue;

                    other.get_data(buffer, offset, other);
                }
            
                m_server.send(pair.first, buffer.data(), buffer.size(), websocketpp::frame::opcode::binary);
            }

            for(auto hdl: dels) {
                m_connections.erase(hdl);
            }

            dels.clear();
        }
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
    
    std::unordered_map<connection_hdl, cursor_data, connection_hdl_hash, connection_hdl_equal> m_connections;

    std::unordered_set<connection_hdl, connection_hdl_hash, connection_hdl_equal> dels;

    condition_variable m_cursors_cond;
};

// some stuff
std::function<void(int)> signal_handler_callback;

void signal_handler(int signal) {
    if (signal_handler_callback) {
        signal_handler_callback(signal);
    }
}

int main() {
    cursors_server server;
    
    signal_handler_callback = [&server](int signum) {
        std::cout << "Signal " << signum << " received. Exiting cleanly";
        server.shutdown();
        exit(signum);
    };

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    websocketpp::lib::thread t(&cursors_server::cursor_loop, &server);
    
    server.run(8081);

    t.join();
}
