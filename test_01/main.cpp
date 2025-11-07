#include "engine/easy.h"
#include "engine/arctic_platform_tcpip.h"
#include "engine/optionparser.h"
#include "circular_buffer.h"
#include <vector>
#include <deque>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <cstring>
#include <future>
#include <queue>
#include <condition_variable>
#include <mutex>
#include "engine/optionparser.h"

using namespace arctic;  // NOLINT

// Network configuration (defaults, can be overridden by command line)
std::string g_server_address = "127.0.0.1";
uint16_t g_server_port = 1499;
uint32_t g_client_connections = 1;

constexpr Ui32 kMaxConnections = 32500;
constexpr Ui32 kConnectionsPerEmulator = 16250;
constexpr Ui32 kGameTickMs = 50;  // 20 ticks per second

constexpr size_t kClientInputSize = 16;     // 16 bytes
constexpr size_t kServerStateSize = 43;     // 43 bytes
constexpr Ui32 kStatesPerSecond = 20;       // 20 messages per second

#pragma pack(push, 1)
// Client -> Server: Input message
struct ClientInput {
  Ui8 data[kClientInputSize];
};
// Server -> Client: State message
struct ServerState {
  Ui8 data[kServerStateSize];
};

#pragma pack(pop)


// Performance metrics
struct Metrics {
  std::atomic<Ui64> messages_received{0};
  std::atomic<Ui64> bytes_sent{0};
  std::atomic<Ui64> bytes_received{0};
  std::atomic<Ui64> connection_errors{0};
  std::atomic<Ui64> tick_count{0};
  
  // Separate counters for server and client
  std::atomic<Ui64> server_active_connections{0};
  std::atomic<Ui64> client_active_connections{0};
  
  // Speed measurement
  std::atomic<Ui64> last_bytes_sent{0};
  std::atomic<Ui64> last_bytes_received{0};
  std::atomic<double> last_speed_time{0.0};
  std::atomic<double> send_speed_mbps{0.0};
  std::atomic<double> recv_speed_mbps{0.0};
};

Metrics g_metrics;
Font g_font;

// Simple thread pool for parallel connection processing
class ThreadPool {
public:
  ThreadPool(size_t num_threads) : stop_(false), active_tasks_(0) {
    for (size_t i = 0; i < num_threads; ++i) {
      workers_.emplace_back([this] {
        while (true) {
          std::function<void()> task;
          {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
            active_tasks_++; // Increment active tasks counter
          }
          
          task(); // Execute task
          
          {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            active_tasks_--; // Decrement active tasks counter
          }
          completion_condition_.notify_all(); // Notify wait_all()
        }
      });
    }
  }

  ~ThreadPool() {
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      stop_ = true;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_) {
      worker.join();
    }
  }

  template<class F>
  void enqueue(F&& f) {
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      tasks_.emplace(std::forward<F>(f));
    }
    condition_.notify_one();
  }

  void wait_all() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    completion_condition_.wait(lock, [this] { 
      return tasks_.empty() && active_tasks_ == 0; 
    });
  }

private:
  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex queue_mutex_;
  std::condition_variable condition_;
  std::condition_variable completion_condition_;
  std::atomic<int> active_tasks_; // Track active tasks
  bool stop_;
};

// Update network speed measurements
void UpdateNetworkSpeed() {
  double current_time = Time();
  double time_diff = current_time - g_metrics.last_speed_time.load();
  
  // Update speed every second
  if (time_diff >= 1.0) {
    Ui64 current_sent = g_metrics.bytes_sent.load();
    Ui64 current_received = g_metrics.bytes_received.load();
    Ui64 last_sent = g_metrics.last_bytes_sent.load();
    Ui64 last_received = g_metrics.last_bytes_received.load();
    
    // Calculate speed in MB/s
    double sent_diff = static_cast<double>(current_sent - last_sent);
    double recv_diff = static_cast<double>(current_received - last_received);
    
    g_metrics.send_speed_mbps = (sent_diff / time_diff) / (1024.0 * 1024.0);
    g_metrics.recv_speed_mbps = (recv_diff / time_diff) / (1024.0 * 1024.0);
    
    // Update last values
    g_metrics.last_bytes_sent = current_sent;
    g_metrics.last_bytes_received = current_received;
    g_metrics.last_speed_time = current_time;
  }
}

// Connection state for each client
enum ConnectionState {
  kConnStateDisconnected = 0,
  kConnStateConnecting,
  kConnStateActive,
  kConnStateError
};

class ClientConnection {
private:
  ConnectionSocket socket_;
  ConnectionState state_;
  Ui32 client_id_;
  CircularBuffer send_buffer_;
  CircularBuffer recv_buffer_;
  ClientInput input_;

public:
  ClientConnection(Ui32 client_id) 
    : state_(kConnStateDisconnected)
    , client_id_(client_id)
    , send_buffer_(1024)   // 1KB send buffer (power of 2)
    , recv_buffer_(16384)  // 16KB receive buffer (power of 2)
    {
      memset(input_.data, 1, kClientInputSize);
    }

  bool Connect() {
    // Reset state and create new socket
    state_ = kConnStateDisconnected;
    send_buffer_.Clear();
    recv_buffer_.Clear();
    socket_ = ConnectionSocket(AddressFamily::kIpV4, SocketProtocol::kTcp);
    if (!socket_.IsValid()) {
      state_ = kConnStateError;
      g_metrics.connection_errors++;
      return false;
    }

    SocketResult res = socket_.SetSoNonblocking(true);
    if (res != kSocketOk) {
      state_ = kConnStateError;
      g_metrics.connection_errors++;
      return false;
    }

    res = socket_.Connect(g_server_address.c_str(), g_server_port);
    state_ = kConnStateConnecting;
    *Log() << "Client " << client_id_ << " attempting to connect...";
    return true;
  }

  void SendInput() {
    if (state_ != kConnStateActive && state_ != kConnStateConnecting) {
      return;
    }
    send_buffer_.Write(&input_, kClientInputSize);
  }

  void Update() {
    if (state_ == kConnStateDisconnected || state_ == kConnStateError) {
      return;
    }
    // If we're in connecting state, send initial message to establish connection
    if (state_ == kConnStateConnecting && send_buffer_.IsEmpty()) {
      SendInput();
    }
    
    // Try to send data using continuous region for efficiency
    const uint8_t* send_ptr;
    size_t send_size;
    if (send_buffer_.GetContinuousReadRegion(&send_ptr, &send_size)) {
      size_t written = 0;
      SocketResult res = socket_.Write(reinterpret_cast<const char*>(send_ptr), send_size, &written);
      if (res == kSocketOk && written > 0) {
        g_metrics.bytes_sent += written;
        send_buffer_.AdvanceReadPosition(written);
      } else if (res == kSocketConnectionReset) {
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        return;
      } else if (res == kSocketError) {
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        return;
      }
    }

    // Try to receive data using continuous region for efficiency
    uint8_t* recv_ptr;
    size_t recv_space;
    if (recv_buffer_.GetContinuousWriteRegion(&recv_ptr, &recv_space)) {
      size_t read = 0;
      SocketResult read_res = socket_.Read(reinterpret_cast<char*>(recv_ptr), recv_space, &read);
      if (read_res == kSocketOk && read > 0) {
        recv_buffer_.AdvanceWritePosition(read);
        g_metrics.bytes_received += read;
        ProcessReceivedData();
      } else if (read_res == kSocketConnectionReset) {
        // Only handle connection reset - ignore all other errors
        state_ = kConnStateError;
        g_metrics.connection_errors++;
      } else if (read_res == kSocketError) {
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        return;
      }
    }
  }

  void ProcessReceivedData() {
    while (recv_buffer_.GetAvailableReadData() >= kServerStateSize) {
      ServerState state;
      size_t read = recv_buffer_.Read(&state, kServerStateSize);
      if (read == kServerStateSize) {
        ProcessServerState(&state);
        g_metrics.messages_received++;
      } else {
        *Log() << "ERROR: ProcessReceivedData: read " << read << " bytes, expected " << kServerStateSize << " bytes";
        break;
      }
    }
  }

  void ProcessServerState(ServerState* state) {
    if (state_ == kConnStateConnecting) {
      state_ = kConnStateActive;
      g_metrics.client_active_connections++;
      *Log() << "Client " << client_id_ << " became active (total: " << g_metrics.client_active_connections.load() << ")";
    }
  }

  ConnectionState GetState() const { return state_; }
  Ui32 GetClientId() const { return client_id_; }
  bool IsActive() const { return state_ == kConnStateActive; }
};

// Server-side client connection handler
class ServerClientConnection {
private:
  ConnectionSocket socket_;
  ConnectionState state_;
  Ui32 client_id_;
  CircularBuffer send_buffer_;
  CircularBuffer recv_buffer_;

public:
  ServerClientConnection(ConnectionSocket&& socket, Ui32 client_id) 
    : socket_(std::move(socket))
    , state_(kConnStateConnecting)
    , client_id_(client_id)
    , send_buffer_(1024*16)   // 4KB send buffer (power of 2)
    , recv_buffer_(1024)   // 4KB receive buffer (power of 2)
  {
  }

  void Update() {
    if (state_ == kConnStateError) return;

    // Try to receive data using continuous region
    uint8_t* recv_ptr;
    size_t recv_space;
    if (recv_buffer_.GetContinuousWriteRegion(&recv_ptr, &recv_space)) {
      size_t read = 0;
      SocketResult read_res = socket_.Read(reinterpret_cast<char*>(recv_ptr), recv_space, &read);
      if (read_res == kSocketOk && read > 0) {
        recv_buffer_.AdvanceWritePosition(read);
        g_metrics.bytes_received += read;
        ProcessReceivedData();
      } else if (read_res == kSocketConnectionReset) {
        state_ = kConnStateError;
        g_metrics.connection_errors++;
      } else if (read_res == kSocketError) {
        state_ = kConnStateError;
        g_metrics.connection_errors++;
      }
    }

    // Try to send data using continuous region
    const uint8_t* send_ptr;
    size_t send_size;
    if (send_buffer_.GetContinuousReadRegion(&send_ptr, &send_size)) {
      size_t written = 0;
      SocketResult write_res = socket_.Write(reinterpret_cast<const char*>(send_ptr), send_size, &written);
      if (write_res == kSocketOk && written > 0) {
        send_buffer_.AdvanceReadPosition(written);
        g_metrics.bytes_sent += written;
      } else if (write_res == kSocketConnectionReset) {
        state_ = kConnStateError;
        g_metrics.connection_errors++;
      }
    }
  }

  void ProcessReceivedData() {
    while (recv_buffer_.GetAvailableReadData() >= kClientInputSize) {
      ClientInput input;
      size_t read = recv_buffer_.Read(&input, kClientInputSize);
      if (read == kClientInputSize) {
        ProcessClientInput(&input);
        g_metrics.messages_received++;
      } else {
        *Log() << "ERROR: ProcessReceivedData: read " << read << " bytes, expected " << kClientInputSize << " bytes";
        break;
      }
    }
  }

  void ProcessClientInput(ClientInput* input) {
    if (state_ == kConnStateConnecting) {
      state_ = kConnStateActive;
      
      *Log() << "Server accepted client " << client_id_;
    }
  }

  void SendServerState() {
    if (state_ != kConnStateActive && state_ != kConnStateConnecting) return;

    ServerState server_state;
    memset(server_state.data, 1, kServerStateSize);

    send_buffer_.Write(&server_state, kServerStateSize);
  }


  ConnectionState GetState() const { return state_; }
  Ui32 GetClientId() const { return client_id_; }
  bool IsActive() const { return state_ == kConnStateActive; }
};

// Server implementation
class LoadTestServer {
private:
  ListenerSocket listener_;
  std::vector<std::unique_ptr<ServerClientConnection>> connections_;
  double last_tick_time_;
  double last_stats_time_;
  std::unique_ptr<ThreadPool> thread_pool_;
  size_t num_threads_;

public:
  LoadTestServer() : last_tick_time_(0.0), last_stats_time_(0.0) {
    num_threads_ = 2;
    thread_pool_ = std::make_unique<ThreadPool>(num_threads_);
    *Log() << "Created thread pool with " << num_threads_ << " threads";
  }

  bool Start() {
    listener_ = ListenerSocket(AddressFamily::kIpV4, SocketProtocol::kTcp);
    if (!listener_.IsValid()) {
      *Log() << "Failed to create listener socket: " << listener_.GetLastError();
      return false;
    }

    SocketResult res = listener_.SetSoLinger(false, 0);
    if (res != kSocketOk) {
      *Log() << "SetSoLinger failed: " << listener_.GetLastError();
      return false;
    }

    res = listener_.Bind(g_server_address.c_str(), g_server_port, 1000);
    if (res != kSocketOk) {
      *Log() << "Bind failed: " << listener_.GetLastError();
      return false;
    }

    res = listener_.SetSoNonblocking(true);
    if (res != kSocketOk) {
      *Log() << "SetSoNonblocking failed: " << listener_.GetLastError();
      return false;
    }

    *Log() << "Server started on " << g_server_address << ":" << g_server_port;
    return true;
  }

  void Update() {
    double current_time = Time();
    
    AcceptNewConnections();

    // Send game state updates at defined frequency (50ms = 20 Hz)
    if (current_time - last_tick_time_ >= kGameTickMs / 1000.0) {
      g_metrics.tick_count++;
      last_tick_time_ = current_time;
      SendGameStateUpdates();
    }

    UpdateConnections();
    RemoveDisconnectedConnections();
    
    
    
    if (current_time - last_stats_time_ >= 10.0) {
      LogStatistics();
      last_stats_time_ = current_time;
    }
  }
  
  void GetConnectionStats(int& connecting, int& error, int& active) {
    connecting = error = active = 0;
    for (const auto& conn : connections_) {
      switch (conn->GetState()) {
        case kConnStateConnecting: connecting++; break;
        case kConnStateActive: active++; break;
        case kConnStateError: error++; break;
        default: break;
      }
    }
  }

private:
  void AcceptNewConnections() {
    int accepted_this_call = 0;
    for (int i = 0; i < 1000; i++) {
      ConnectionSocket client_socket = listener_.Accept();
      if (!client_socket.IsValid()) {
        *Log() << "Accept new connections failed: " << client_socket.GetLastError();
      } else {
        SocketResult res = client_socket.SetSoNonblocking(true);
        if (res != kSocketOk) {
          *Log() << "ERROR: Failed to set client socket non-blocking";
        } else {
          accepted_this_call++;
          connections_.emplace_back(std::make_unique<ServerClientConnection>(std::move(client_socket), connections_.size() - 1));
        }
      }
    }
    if (accepted_this_call > 0) {
      *Log() << "Server accepted " << accepted_this_call << " connections";
    }
  }

  void UpdateConnections() {
    // Update all connections using thread pool
    size_t count = connections_.size();
    if (count == 0) return;
    
    // Calculate work per thread
    size_t connections_per_thread = (count + num_threads_ - 1) / num_threads_;
    
    // Submit work to thread pool
    for (size_t thread_id = 0; thread_id < num_threads_; ++thread_id) {
      size_t start_idx = thread_id * connections_per_thread;
      size_t end_idx = std::min(start_idx + connections_per_thread, count);
      
      if (start_idx < count) {
        thread_pool_->enqueue([this, start_idx, end_idx]() {
          for (size_t i = start_idx; i < end_idx; ++i) {
            connections_[i]->Update();
          }
        });
      }
    }
    
    // Wait for all threads to complete
    thread_pool_->wait_all();
  }

  void RemoveDisconnectedConnections() {
    // Remove disconnected connections
    size_t before_count = connections_.size();
    connections_.erase(
      std::remove_if(connections_.begin(), connections_.end(),
        [](const std::unique_ptr<ServerClientConnection>& conn) {
          if (conn->GetState() == kConnStateError) {
            *Log() << "Removed error connection " << conn->GetClientId();
            return true;
          }
          return false;
        }),
      connections_.end());
    
    if (connections_.size() != before_count) {
      *Log() << "Removed " << (before_count - connections_.size()) << " disconnected clients";
    }
  }

  void SendGameStateUpdates() {
    for (auto& conn : connections_) {
      conn->SendServerState();
    }
  }

  void LogStatistics() {
    int connecting = 0, active = 0, error = 0;
    for (const auto& conn : connections_) {
      switch (conn->GetState()) {
        case kConnStateConnecting: connecting++; break;
        case kConnStateActive: active++; break;
        case kConnStateError: error++; break;
        default: break;
      }
    }
    
    *Log() << "=== Load Test Statistics ===";
    *Log() << "Total connections: " << connections_.size();
    *Log() << "  - Connecting: " << connecting;
    *Log() << "  - Error: " << error;
    *Log() << "Messages received: " << g_metrics.messages_received.load();
    *Log() << "Bytes sent: " << g_metrics.bytes_sent.load();
    *Log() << "Bytes received: " << g_metrics.bytes_received.load();
    *Log() << "Connection errors: " << g_metrics.connection_errors.load();
    *Log() << "Ticks processed: " << g_metrics.tick_count.load();
    *Log() << "============================";
  }
};

// Client emulator that manages multiple connections
class ClientEmulator {
private:
  std::vector<std::unique_ptr<ClientConnection>> connections_;
  Ui32 base_client_id_;
  Ui32 connection_count_;
  Ui32 connections_created_;
  double last_connect_time_;
  double last_input_time_;
  std::unique_ptr<ThreadPool> thread_pool_;
  size_t num_threads_;

public:
  ClientEmulator(Ui32 base_id, Ui32 count) 
    : base_client_id_(base_id)
    , connection_count_(count)
    , connections_created_(0)
    , last_connect_time_(0.0)
    , last_input_time_(0.0) {
    
    num_threads_ = 2;
    thread_pool_ = std::make_unique<ThreadPool>(num_threads_);
    *Log() << "ClientEmulator: Created thread pool with " << num_threads_ << " threads";
    
    connections_.reserve(count);
    for (Ui32 i = 0; i < count; ++i) {
      connections_.emplace_back(std::make_unique<ClientConnection>(base_id + i));
    }
  }

  void ConnectAll() {
    double current_time = Time();
    if (current_time - last_connect_time_ >= 0.05) { // 50ms between batches (faster retry)
      int batch_size = 10;
      int connected_this_batch = 0;
      
      
      
      // retry failed connections
      int retries_this_batch = 0;
      for (auto& conn : connections_) {
        if (conn->GetState() == kConnStateError && retries_this_batch < batch_size) {
          conn->Connect();
          retries_this_batch++;
          connected_this_batch++;
          *Log() << "Retrying error connection " << conn->GetClientId();
        }
        if (conn->GetState() == kConnStateDisconnected && retries_this_batch < batch_size) {
          conn->Connect();
          retries_this_batch++;
          connected_this_batch++;
          *Log() << "Retrying disconnected connection " << conn->GetClientId();
        }
      }

      batch_size -= connected_this_batch;
      for (int i = 0; i < batch_size && connections_created_ < connection_count_; ++i) {
        connections_[connections_created_]->Connect();
        connections_created_++;
        connected_this_batch++;
      }
      
      last_connect_time_ = current_time;
      *Log() << "Connected " << connections_created_ << "/" << connection_count_ << " clients (retried " << retries_this_batch << " failed)";
    }
  }

  void Update() {
    double current_time = Time(); 
    ConnectAll();
    
    // Update all connections using thread pool
    size_t count = connections_.size();
    if (count > 0) {
      // Calculate work per thread
      size_t connections_per_thread = (count + num_threads_ - 1) / num_threads_;
      
      // Submit work to thread pool
      for (size_t thread_id = 0; thread_id < num_threads_; ++thread_id) {
        size_t start_idx = thread_id * connections_per_thread;
        size_t end_idx = std::min(start_idx + connections_per_thread, count);
        
        if (start_idx < count) {
          thread_pool_->enqueue([this, start_idx, end_idx]() {
            for (size_t i = start_idx; i < end_idx; ++i) {
              connections_[i]->Update();
            }
          });
        }
      }
      
      // Wait for all threads to complete
      thread_pool_->wait_all();
    }
    
    // Send client input messages once per second
    if (current_time - last_input_time_ >= 1.0) {
      for (auto& conn : connections_) {
        if (conn->IsActive() || conn->GetState() == kConnStateConnecting) {
          conn->SendInput();
        }
      }
      last_input_time_ = current_time;
    }
  }
  
  void GetConnectionStats(int& active, int& connecting, int& error, int& total) {
    active = connecting = error = 0;
    total = connections_.size();
    
    for (const auto& conn : connections_) {
      switch (conn->GetState()) {
        case kConnStateActive: active++; break;
        case kConnStateConnecting: connecting++; break;
        case kConnStateError: error++; break;
        default: break;
      }
    }
  }
};

// Test modes
enum TestMode {
  kTestModeNotSet = 0,
  kTestModeServer,
  kTestModeClientEmulator,
  kTestModeRealClient
};

TestMode g_test_mode = kTestModeNotSet;
std::unique_ptr<LoadTestServer> g_server;
std::unique_ptr<ClientEmulator> g_client;

void DrawUI() {
  Clear();
  
  // Update network speed measurements
  UpdateNetworkSpeed();
  
  // Title at top (high from bottom edge)
  char title[256];
  snprintf(title, sizeof(title), "MMORPG Network Load Test - Mode: %s", 
           g_test_mode == kTestModeNotSet ? "Mode not set, will default to server" :
           g_test_mode == kTestModeServer ? "Server" : 
           g_test_mode == kTestModeClientEmulator ? "Client Emulator" : "Real Client");
  g_font.Draw(title, 20, ScreenSize().y - 50, kTextOriginTop);
  
  // Statistics - start from top and go down
  int y_pos = ScreenSize().y - 100;  // Start high
  int line_height = 30;
  
  char line[256];
  if (g_test_mode == kTestModeServer && g_server) {
    // Show detailed connection statistics for server
    int connecting, error, active;
    g_server->GetConnectionStats(connecting, error, active);
    
    snprintf(line, sizeof(line), "Connections: %d connecting, %d error, %d active (total: %d)", 
             connecting, error, active, connecting+error+active);
    g_font.Draw(line, 20, y_pos, kTextOriginTop);
    y_pos -= line_height;
  } else if (g_test_mode == kTestModeServer || g_test_mode == kTestModeNotSet) {
    // Server not started yet
    snprintf(line, sizeof(line), "Server not started - Press SPACE to start");
    g_font.Draw(line, 20, y_pos, kTextOriginTop);
    y_pos -= line_height;
  } else if (g_client) {
    // Show detailed connection statistics for client
    int active, connecting, error, total;
    g_client->GetConnectionStats(active, connecting, error, total);
    
    snprintf(line, sizeof(line), "Connections: %d active, %d connecting, %d error (total: %d)", 
             active, connecting, error, total);
    g_font.Draw(line, 20, y_pos, kTextOriginTop);
    y_pos -= line_height;
  } else {
    // Client not started yet
    const char* mode_name = (g_test_mode == kTestModeClientEmulator) ? "Client Emulator" : "Real Client";
    snprintf(line, sizeof(line), "%s not started - Press SPACE to start", mode_name);
    g_font.Draw(line, 20, y_pos, kTextOriginTop);
    y_pos -= line_height;
  }
  
  snprintf(line, sizeof(line), "Messages Received: %llu", g_metrics.messages_received.load());
  g_font.Draw(line, 20, y_pos, kTextOriginTop);
  y_pos -= line_height;
  
  snprintf(line, sizeof(line), "Bytes Sent: %llu (%.2f MB/s)", 
           g_metrics.bytes_sent.load(), g_metrics.send_speed_mbps.load());
  g_font.Draw(line, 20, y_pos, kTextOriginTop);
  y_pos -= line_height;
  
  snprintf(line, sizeof(line), "Bytes Received: %llu (%.2f MB/s)", 
           g_metrics.bytes_received.load(), g_metrics.recv_speed_mbps.load());
  g_font.Draw(line, 20, y_pos, kTextOriginTop);
  y_pos -= line_height;
  
  snprintf(line, sizeof(line), "Connection Errors: %llu", g_metrics.connection_errors.load());
  g_font.Draw(line, 20, y_pos, kTextOriginTop);
  y_pos -= line_height;
  
  snprintf(line, sizeof(line), "Ticks: %llu", g_metrics.tick_count.load());
  g_font.Draw(line, 20, y_pos, kTextOriginTop);
  y_pos -= line_height;
  
  if (g_test_mode == kTestModeClientEmulator) {
    snprintf(line, sizeof(line), "Target: %d connections, 1 msg/sec client, 20 Hz server", kConnectionsPerEmulator);
  } else if (g_test_mode == kTestModeRealClient) {
    snprintf(line, sizeof(line), "Target: %d connection(s), 1 msg/sec client, 20 Hz server", g_client_connections);
  } else {
    snprintf(line, sizeof(line), "Target: 32500 total connections, 20 Hz server");
  }
  g_font.Draw(line, 20, y_pos, kTextOriginTop);
  y_pos -= line_height;
  
  snprintf(line, sizeof(line), "Protocol: Client 16 bytes -> Server 43 bytes");
  g_font.Draw(line, 20, y_pos, kTextOriginTop);
  
  // Instructions at bottom (leave space for ~1 line at bottom)
  
  y_pos = 55;
  g_font.Draw("ESC to exit", 20, y_pos, kTextOriginTop);
  y_pos += 25;
  if (g_test_mode == kTestModeNotSet) {
    g_font.Draw("Press 1 for Server mode", 20, y_pos, kTextOriginTop);
    y_pos += 25;
    g_font.Draw("Press 2 for Client Emulator mode", 20, y_pos, kTextOriginTop);
    y_pos += 25;
    g_font.Draw("Press 3 for Real Client mode", 20, y_pos, kTextOriginTop);
    y_pos += 25;
  }
  g_font.Draw("Controls:", 20, y_pos, kTextOriginTop);
  ShowFrame();
}

void EasyMain() {
  // Parse command line arguments
  Si32 argc = GetEngine()->GetArgc();
  const char* const* argv = GetEngine()->GetArgv();
  
  arctic::OptionParser parser("MMORPG Network Load Test Tool");
  
  // Add command line options
  parser.AddOption("--help", "-h")
      .Help("Display this help message and exit.")
      .NoArguments();
      
  parser.AddOption("--mode", "-m")
      .Help("Test mode: server or client")
      .SingleArgument();
      
  parser.AddOption("--address", "-a")
      .Help("Server address to connect to")
      .DefaultValue("127.0.0.1")
      .ArgumentType<std::string>()
      .SingleArgument();
      
  parser.AddOption("--port", "-p")
      .Help("Server port")
      .DefaultValue("1499")
      .ArgumentType<uint16_t>()
      .SingleArgument();
      
  parser.AddOption("--connections", "-c")
      .Help("Number of connections for client mode")
      .DefaultValue("16250")
      .ArgumentType<uint32_t>()
      .SingleArgument();
  
  // Parse arguments
  bool is_ok = parser.ParseArgcArgv(argc, const_cast<char const**>(argv));
  if (!is_ok) {
    std::cerr << "Error parsing arguments: " << parser.get_last_error();
    return;
  }
  
  if (parser.HasValue("help")) {
    std::cout << parser.Help();
    return;
  }
  
  if (!parser.CheckForMissingArgs()) {
    std::cerr << "Error: " << parser.get_last_error();
    return;
  }
  
  if (parser.HasValue("mode")) {
    std::string mode = parser.GetValue<std::string>("mode", "");
    if (mode == "server") {
      g_test_mode = kTestModeServer;
      *Log() << "Starting in server mode";
    } else if (mode == "client") {
      g_test_mode = kTestModeRealClient;
      *Log() << "Starting in client mode";
    } else {
      std::cerr << "Unknown mode: \"" << mode << "\". Valid modes: server, client";
      return;
    }
  }

  // Get server address and port
  g_server_address = parser.GetValue<std::string>("address", "127.0.0.1");
  g_server_port = parser.GetValue<uint16_t>("port", 1499);
  
  // Get number of connections for client mode
  g_client_connections = parser.GetValue<uint32_t>("connections", 1);
  
  *Log() << "Server address: " << g_server_address << ":" << g_server_port;
  if (g_test_mode == kTestModeRealClient) {
    *Log() << "Client connections: " << g_client_connections;
  }

  g_font.Load("data/arctic_one_bmf.fnt");
  ResizeScreen(800, 600);
  SetVSync(false);
  
  // Initialize speed measurement
  g_metrics.last_speed_time = Time();
  
  *Log() << "MMORPG Network Load Test Starting";
  *Log() << "Target: " << kMaxConnections << " connections";
  *Log() << "Game tick rate: " << (1000 / kGameTickMs) << " Hz";
  
  while (!IsKeyDownward(kKeyEscape)) {
    // Handle mode switching
    if (g_test_mode == kTestModeNotSet) {
      if (IsKeyDownward(kKey1)) {
        g_test_mode = kTestModeServer;
        g_server.reset();
        g_client.reset();
      } else if (IsKeyDownward(kKey2)) {
        g_test_mode = kTestModeClientEmulator;
        g_server.reset();
        g_client.reset();
      } else if (IsKeyDownward(kKey3)) {
        g_test_mode = kTestModeRealClient;
        g_server.reset();
        g_client.reset();
      }
    }
    
    // start/connect
    if (g_test_mode != kTestModeNotSet) {
      if (g_test_mode == kTestModeServer && !g_server) {
        g_test_mode = kTestModeServer;
        g_server = std::make_unique<LoadTestServer>();
        if (g_server->Start()) {
          *Log() << "Server started successfully";
        } else {
          *Log() << "Failed to start server";
          g_server.reset();
        }
      } else if (g_test_mode == kTestModeClientEmulator && !g_client) {
        *Log() << "Starting client emulator...";
        g_client = std::make_unique<ClientEmulator>(0, kConnectionsPerEmulator);
        *Log() << "Client emulator started, connecting " << kConnectionsPerEmulator << " clients gradually...";
      } else if (g_test_mode == kTestModeRealClient && !g_client) {
        *Log() << "Starting real client...";
        g_client = std::make_unique<ClientEmulator>(0, g_client_connections);
        *Log() << "Real client started, connecting " << g_client_connections << " client(s)...";
      }
    }
    
    // Update active components
    for (int i = 0; i < 20; i++) {
      if (g_server) {
        g_server->Update();
      }
      if (g_client) {
        g_client->Update();
      }
    }
    
    DrawUI();
  }
  
  *Log() << "Load test completed";
}

#ifdef ARCTIC_NO_MAIN
namespace arctic {
  extern std::string PrepareInitialPath();
};

int main(int argc, char **argv) {
  std::string initial_path = arctic::PrepareInitialPath();
  arctic::StartLogger();
  arctic::HeadlessPlatformInit();
  arctic::GetEngine()->SetArgcArgv(argc,
    const_cast<const char **>(argv));

  arctic::GetEngine()->SetInitialPath(initial_path);
  arctic::GetEngine()->HeadlessInit();

  EasyMain();

  return 0;
}
#endif  // ARCTIC_NO_MAIN