#define ASIO_STANDALONE

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <unordered_map>
#include <csignal>
#include <vector>

// A structure representing a cursor on the screen.
struct cursor_data {
    /* 
      we don't need the id as a member.
      the only place where raw cursor data is stored and being accessed from is the unordered map.
      if you're accessing a cursor, it means you already know the id which is the key in the map.
    */

    // the cursor's position on it's own screen (pageX, pageY)
    uint32_t x, y;

    // color support?
    uint8_t redgreenblue[3];
    // nickname support?
    std::string nick;

    std::string world;

    // we need screen data to calculate the position relative to the other cursor's screen
    uint16_t screen_width = 1370, screen_height = 600; // the page's scrollable area (documentElement.scrollWidth, documentElement.scrollHeight)

    // partial
    std::vector<uint8_t>& get_partial_data(std::vector<uint8_t>& buffer, size_t& offset, const cursor_data& other_cursor) {
        float other_x = x * other_cursor.screen_width / 4294967295; // Ultra precision or inefficient calculation? idk
        float other_y = y * other_cursor.screen_height / 4294967295;
        
        std::memcpy(&buffer[offset], &other_x, 4);
        offset += 4;

        std::memcpy(&buffer[offset], &other_y, 4);
        offset += 4;

        return buffer;
    }

    // appear
    std::vector<uint8_t>& get_full_data(std::vector<uint8_t>& buffer, size_t& offset, const cursor_data& other_cursor) {
        uint32_t other_x = x * other_cursor.screen_width / 4294967295;
        uint32_t other_y = y * other_cursor.screen_height / 4294967295;
        
        std::memcpy(&buffer[offset], &other_x, 4);
        offset += 4;

        std::memcpy(&buffer[offset], &other_y, 4);
        offset += 4;

        std::memcpy(&buffer[offset], nick.c_str(), nick.length() + 1); // +1 to include the null terminator
        offset += nick.length() + 1;

        std::memcpy(&buffer[offset], redgreenblue, 3);
        offset += 3;
            
        return buffer;
    }
};


// now get to the webSocket server
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

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

    run(uint16_t port) {
        m_server.listen(port);
        m_server.start_accept();

        m_server.get_alog().write(websocketpp::log::alevel::app, "Server listening on port " + std::to_string(port));

        try {
            m_server.run();
        } catch(const std::exception &e) {
            std::cout << e.what() << std::endl;
        }
    }

    shutdown() {
        m_connections.clear();
        m_server.stop_listening();
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
        s.shutdown();
        exit(signum);
    };

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server.run(8081);
}
