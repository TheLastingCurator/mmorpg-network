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
#include <sstream>
#include <iomanip>
#include <algorithm>
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

bool g_is_headless = false;

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
  double last_input_send_time_;

public:
  ClientConnection(Ui32 client_id) 
    : state_(kConnStateDisconnected)
    , client_id_(client_id)
    , send_buffer_(1024)   // 1KB send buffer (power of 2)
    , recv_buffer_(16384)  // 16KB receive buffer (power of 2)
    , last_input_send_time_(0.0)
    {
      memset(input_.data, 1, kClientInputSize);
    }

  std::string GetLastError() const {
    return socket_.GetLastError();
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
    if (res != SocketResult::kSocketOk) {
      *Log() << "C SetSoNonblocking failed for client " << client_id_ << ": " << socket_.GetLastError();
      state_ = kConnStateError;
      g_metrics.connection_errors++;
      return false;
    }

    SocketConnectResult connect_res = socket_.Connect(g_server_address.c_str(), g_server_port);
    state_ = kConnStateConnecting;
    if (connect_res == SocketConnectResult::kSocketOk) {
      state_ = kConnStateActive;
      *Log() << "C Connect: Client " << client_id_ << " became active after async connect";
      return true;
    } else if (connect_res == SocketConnectResult::kSocketConnectionInProgress) {
      state_ = kConnStateConnecting;
      return true;
    } else {
      *Log() << "C Connect failed for client " << client_id_ << ": " << socket_.GetLastError();
      state_ = kConnStateError;
      g_metrics.connection_errors++;
      return false;
    }
  }

  void SendInput() {
    if (state_ != kConnStateActive) {
      return;
    }
    send_buffer_.Write(&input_, kClientInputSize);
  }

  void Update() {
    if (state_ == kConnStateDisconnected || state_ == kConnStateError) {
      return;
    }
    // Handle connection in progress
    if (socket_.GetState() == SocketState::kConnectionInProgress) {
      socket_.UpdateConnectionInProgressState();
      if (socket_.GetState() == SocketState::kDisconnected) {
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        return;
      }
      if (socket_.GetState() == SocketState::kConnectionInProgress) {
        // Still connecting, don't try to send data
        return;
      }
      // If socket became connected, update client state
      if (socket_.GetState() == SocketState::kConnected && state_ == kConnStateConnecting) {
        state_ = kConnStateActive;
        *Log() << "C Update: Client " << client_id_ << " became active after async connect";
      }
    }
    
    // Only proceed with data operations if socket is connected and client is active
    if (socket_.GetState() != SocketState::kConnected || state_ != kConnStateActive) {
      return;
    }
    
    // Send input only once per second when socket is connected
    double current_time = GetEngine()->GetTime();
    if (current_time - last_input_send_time_ >= 1.0) {
      SendInput();
      last_input_send_time_ = current_time;
    }
    // Try to send data using continuous region for efficiency
    const uint8_t* send_ptr;
    size_t send_size;
    if (send_buffer_.GetContinuousReadRegion(&send_ptr, &send_size)) {
      size_t written = 0;
      SocketResult res = socket_.Write(reinterpret_cast<const char*>(send_ptr), send_size, &written);
      if (res == SocketResult::kSocketOk && written > 0) {
        g_metrics.bytes_sent += written;
        send_buffer_.AdvanceReadPosition(written);
      } else if (res == SocketResult::kSocketConnectionReset) {
        *Log() << "C Write failed (kSocketConnectionReset) for client " << client_id_ << ": " << socket_.GetLastError();
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        send_buffer_.Clear();
        return;
      } else if (res == SocketResult::kSocketError) {
        *Log() << "C Write failed (kSocketError) for client " << client_id_ << ": " << socket_.GetLastError();
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        send_buffer_.Clear();
        return;
      }
    }

    // Try to receive data using continuous region for efficiency
    uint8_t* recv_ptr;
    size_t recv_space;
    if (recv_buffer_.GetContinuousWriteRegion(&recv_ptr, &recv_space)) {
      size_t read = 0;
      SocketResult read_res = socket_.Read(reinterpret_cast<char*>(recv_ptr), recv_space, &read);
      if (read_res == SocketResult::kSocketOk && read > 0) {
        recv_buffer_.AdvanceWritePosition(read);
        g_metrics.bytes_received += read;
        ProcessReceivedData();
      } else if (read_res == SocketResult::kSocketConnectionReset) {
        *Log() << "C Read failed (kSocketConnectionReset) for client " << client_id_ << ": " << socket_.GetLastError();
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        recv_buffer_.Clear();
        return;
      } else if (read_res == SocketResult::kSocketError) {
        *Log() << "C Read failed (kSocketError) for client " << client_id_ << ": " << socket_.GetLastError();
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        recv_buffer_.Clear();
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
        *Log() << "C ERROR: ProcessReceivedData: read " << read << " bytes, expected " << kServerStateSize << " bytes";
        break;
      }
    }
  }

  void ProcessServerState(ServerState* state) {
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
    , state_(kConnStateActive)
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
      if (read_res == SocketResult::kSocketOk && read > 0) {
        recv_buffer_.AdvanceWritePosition(read);
        g_metrics.bytes_received += read;
        ProcessReceivedData();
      } else if (read_res == SocketResult::kSocketConnectionReset) {
        *Log() << "S Read failed (kSocketConnectionReset) for client " << client_id_ << ": " << socket_.GetLastError();
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        recv_buffer_.Clear();
      } else if (read_res == SocketResult::kSocketError) {
        *Log() << "S Read failed (kSocketError) for client " << client_id_ << ": " << socket_.GetLastError();
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        recv_buffer_.Clear();
      }
    }

    // Try to send data using continuous region
    const uint8_t* send_ptr;
    size_t send_size;
    if (send_buffer_.GetContinuousReadRegion(&send_ptr, &send_size)) {
      size_t written = 0;
      SocketResult write_res = socket_.Write(reinterpret_cast<const char*>(send_ptr), send_size, &written);
      if (write_res == SocketResult::kSocketOk && written > 0) {
        send_buffer_.AdvanceReadPosition(written);
        g_metrics.bytes_sent += written;
      } else if (write_res == SocketResult::kSocketConnectionReset) {
        *Log() << "S Write failed (kSocketConnectionReset) for client " << client_id_ << ": " << socket_.GetLastError();
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        send_buffer_.Clear();
      } else if (write_res == SocketResult::kSocketError) {
        *Log() << "S Write failed (kSocketError) for client " << client_id_ << ": " << socket_.GetLastError();
        state_ = kConnStateError;
        g_metrics.connection_errors++;
        send_buffer_.Clear();
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
        *Log() << "S ERROR: ProcessReceivedData: read " << read << " bytes, expected " << kClientInputSize << " bytes";
        break;
      }
    }
  }

  void ProcessClientInput(ClientInput* input) {
  }

  void SendServerState() {
    if (state_ != kConnStateActive)
      return;

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
  std::unique_ptr<ThreadPool> thread_pool_;
  size_t num_threads_;

public:
  LoadTestServer() : last_tick_time_(0.0) {
    num_threads_ = 2;
    thread_pool_ = std::make_unique<ThreadPool>(num_threads_);
    *Log() << "S Created thread pool with " << num_threads_ << " threads";
  }

  bool Start() {
    listener_ = ListenerSocket(AddressFamily::kIpV4, SocketProtocol::kTcp);
    if (!listener_.IsValid()) {
      *Log() << "S Failed to create listener socket: " << listener_.GetLastError();
      return false;
    }

    SocketResult res = listener_.SetSoLinger(false, 0);
    if (res != SocketResult::kSocketOk) {
      *Log() << "S SetSoLinger failed: " << listener_.GetLastError();
      return false;
    }

    res = listener_.Bind(g_server_address.c_str(), g_server_port, 1000);
    if (res != SocketResult::kSocketOk) {
      *Log() << "S Bind failed: " << listener_.GetLastError();
      return false;
    }

    res = listener_.SetSoNonblocking(true);
    if (res != SocketResult::kSocketOk) {
      *Log() << "S SetSoNonblocking failed: " << listener_.GetLastError();
      return false;
    }

    res = listener_.SetTcpNoDelay(true);
    if (res != SocketResult::kSocketOk) {
      *Log() << "S SetTcpNoDelay failed: " << listener_.GetLastError();
      return false;
    }

    *Log() << "S Server started on " << g_server_address << ":" << g_server_port;
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
        // *Log() << "Accept new connections failed: " << client_socket.GetLastError();
      } else {
        SocketResult res = client_socket.SetSoNonblocking(true);
        if (res != SocketResult::kSocketOk) {
          *Log() << "S ERROR: Failed to set client socket non-blocking";
        } else {
          res = client_socket.SetTcpNoDelay(true);
          if (res != SocketResult::kSocketOk) {
            *Log() << "S ERROR: Failed to set client socket TCP no delay";
          } else {
            accepted_this_call++;
            connections_.emplace_back(std::make_unique<ServerClientConnection>(std::move(client_socket), connections_.size() - 1));
          }
        }
      }
    }
    if (accepted_this_call > 0) {
      *Log() << "S Server accepted " << accepted_this_call << " connections";
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
            *Log() << "S Removed error connection " << conn->GetClientId();
            return true;
          }
          return false;
        }),
      connections_.end());
  }

  void SendGameStateUpdates() {
    for (auto& conn : connections_) {
      conn->SendServerState();
    }
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
    *Log() << "C ClientEmulator: Created thread pool with " << num_threads_ << " threads";
    
    connections_.reserve(count);
    for (Ui32 i = 0; i < count; ++i) {
      connections_.emplace_back(std::make_unique<ClientConnection>(base_id + i));
    }
  }

  void ConnectAll() {
    double current_time = Time();
    if (current_time - last_connect_time_ >= 0.05) { // 50ms between batches (faster retry)
      int batch_size = 100;
      int connected_this_batch = 0;
      
      
      
      // retry failed connections
      int retries_this_batch = 0;
      for (int i = 0; i < connections_created_; ++i) {
        ClientConnection* conn = connections_[i].get();
        if (conn->GetState() == kConnStateError && retries_this_batch < batch_size) {
          *Log() << "C Retrying error connection " << conn->GetClientId() << " that had error: " << conn->GetLastError();
          conn->Connect();
          retries_this_batch++;
          connected_this_batch++; 
        } else if (conn->GetState() == kConnStateDisconnected && retries_this_batch < batch_size) {
          *Log() << "C Retrying disconnected connection " << conn->GetClientId() << " that had error: " << conn->GetLastError();
          conn->Connect();
          retries_this_batch++;
          connected_this_batch++;
        }
      }

      batch_size -= connected_this_batch;
      for (int i = 0; i < batch_size && connections_created_ < connection_count_; ++i) {
        connections_[connections_created_]->Connect();
        connections_created_++;
        connected_this_batch++;
      }
      
      last_connect_time_ = current_time;
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
        if (conn->IsActive()) {
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

// UI logging
double g_last_ui_log_time = 0.0;

void DrawUI() {
  Clear();
  
  // Update network speed measurements
  UpdateNetworkSpeed();
  
  // Build all text in stringstream
  std::stringstream ui_text;
  
  // Title
  ui_text << "MMORPG Network Load Test - Mode: ";
  if (g_test_mode == kTestModeNotSet) {
    ui_text << "Mode not set, will default to server";
  } else if (g_test_mode == kTestModeServer) {
    ui_text << "Server";
  } else if (g_test_mode == kTestModeClientEmulator) {
    ui_text << "Client Emulator";
  } else {
    ui_text << "Real Client";
  }
  ui_text << "\n\n";
  
  // Connection statistics
  if (g_test_mode == kTestModeServer && g_server) {
    int connecting, error, active;
    g_server->GetConnectionStats(connecting, error, active);
    ui_text << "Connections: " << connecting << " connecting, " << error 
            << " error, " << active << " active (total: " << (connecting+error+active) << ")\n";
  } else if (g_test_mode == kTestModeServer || g_test_mode == kTestModeNotSet) {
    ui_text << "Server not started - Press SPACE to start\n";
  } else if (g_client) {
    int active, connecting, error, total;
    g_client->GetConnectionStats(active, connecting, error, total);
    ui_text << "Connections: " << active << " active, " << connecting 
            << " connecting, " << error << " error (total: " << total << ")\n";
  } else {
    const char* mode_name = (g_test_mode == kTestModeClientEmulator) ? "Client Emulator" : "Real Client";
    ui_text << mode_name << " not started - Press SPACE to start\n";
  }
  
  // Network statistics
  ui_text << "Messages Received: " << g_metrics.messages_received.load() << "\n";
  ui_text << "Bytes Sent: " << g_metrics.bytes_sent.load() 
          << " (" << std::fixed << std::setprecision(2) << g_metrics.send_speed_mbps.load() << " MB/s)\n";
  ui_text << "Bytes Received: " << g_metrics.bytes_received.load() 
          << " (" << std::fixed << std::setprecision(2) << g_metrics.recv_speed_mbps.load() << " MB/s)\n";
  ui_text << "Connection Errors: " << g_metrics.connection_errors.load() << "\n";
  ui_text << "Ticks: " << g_metrics.tick_count.load() << "\n";
  
  // Target configuration
  ui_text << "Target: ";
  if (g_test_mode == kTestModeClientEmulator) {
    ui_text << kConnectionsPerEmulator << " connections, 1 msg/sec client, 20 Hz server";
  } else if (g_test_mode == kTestModeRealClient) {
    ui_text << g_client_connections << " connection(s), 1 msg/sec client, 20 Hz server";
  } else {
    ui_text << "32500 total connections, 20 Hz server";
  }
  ui_text << "\n";
  
  ui_text << "Protocol: Client 16 bytes -> Server 43 bytes\n\n";
  
  if (!g_is_headless) {
    ui_text << "ESC to exit\n";
    if (g_test_mode == kTestModeNotSet) {
      ui_text << "Press 1 for Server mode\n";
      ui_text << "Press 2 for Client Emulator mode\n";
      ui_text << "Press 3 for Real Client mode\n";
    }
    ui_text << "Controls:";
  }
  
  double current_time = Time();
  if (current_time - g_last_ui_log_time >= 5.0) {
    g_last_ui_log_time = current_time;
    *Log() << ui_text.str();
  }

  if (!g_is_headless) {
    g_font.Draw(ui_text.str().c_str(), 20, ScreenSize().y - 50, kTextOriginTop);
    ShowFrame();
  }
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
  if (!g_is_headless) {
    ResizeScreen(800, 600);
    SetVSync(false);
  }
  
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

  g_is_headless = true;
  EasyMain();

  return 0;
}
#endif  // ARCTIC_NO_MAIN