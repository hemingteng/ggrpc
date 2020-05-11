# ggrpc

ggrpc は、マルチスレッドで安全に動作し、グレースフルシャットダウンを可能にした gRPC C++ クライアントです。

## ルール

- `ClientManager` が死んだら、その `ClientManager` で生成した接続は全て安全に切断される
- `Server` が死んだら、その `Server` に紐付けられていたハンドラは全て安全に切断される
- 各クライアントは、そのオブジェクトが参照されている限り安全に読み書きできる
- 各ハンドラは、その `Context()` が参照されている限り安全に読み書きできる
- 強制的に接続を閉じたい場合は `Close()` 関数を呼ぶ
- たとえ `ClientManager` や `Server` のライフタイムが終了していたとしても、クライアントやハンドラのオブジェクトが参照されている限りはアクセスが可能。ただしデータは闇に捨てられていく
- `Close()` 関数を呼んだ後もクライアントやハンドラのオブジェクトが参照されている限りはアクセスが可能。ただしデータは闇に捨てられていく

## シグネチャ

クライアント側:

```cpp
namespace ggrpc {

// 単体リクエスト用
enum class ClientResponseWriterError {
  FINISH,
};

template <class W, class R>
class ClientResponseReader {
 public:
  typedef std::function<std::unique_ptr<grpc::ClientAsyncResponseReader<R>>(
      grpc::ClientContext*, const W&, grpc::CompletionQueue*)>
      RequestFunc;
  typedef std::function<void(R, grpc::Status)> OnResponseFunc;
  typedef std::function<void(ClientResponseWriterError)> OnErrorFunc;

  void SetOnResponse(OnResponseFunc on_response);
  void SetOnError(OnErrorFunc on_error);

  void Request(const W& request);

  void Close();
};

// 双方向ストリーミング用
enum class ClientReaderWriterError {
  CONNECT,
  READ,
  WRITE,
};

template <class W, class R>
class ClientReaderWriter {
 public:
  typedef std::function<std::unique_ptr<grpc::ClientAsyncReaderWriter<W, R>>(
      grpc::ClientContext*, grpc::CompletionQueue*, void*)>
      ConnectFunc;
  typedef std::function<void()> OnConnectFunc;
  typedef std::function<void(R)> OnReadFunc;
  typedef std::function<void(grpc::Status)> OnReadDoneFunc;
  typedef std::function<void(ClientReaderWriterError)> OnErrorFunc;
  typedef std::function<void(W, int64_t)> OnWriteFunc;
  typedef std::function<void()> OnWritesDoneFunc;

  void SetOnConnect(OnConnectFunc on_connect);
  void SetOnRead(OnReadFunc on_read);
  void SetOnReadDone(OnReadDoneFunc on_read_done);
  void SetOnWrite(OnWriteFunc on_write);
  void SetOnWritesDone(OnWritesDoneFunc on_writes_done);
  void SetOnError(OnErrorFunc on_error);

  void Connect();

  void Close();

  void Write(W request, int64_t id = 0);
  void WritesDone();
};

class ClientManager {
public:
  ClientManager(int threads);

  void Start();
  void Shutdown();

  template <class W, class R>
  std::shared_ptr<ClientResponseReader<W, R>> CreateResponseReader(
      typename ClientResponseReader<W, R>::RequestFunc request);

  template <class W, class R>
  std::shared_ptr<ClientReaderWriter<W, R>> CreateReaderWriter(
      typename ClientReaderWriter<W, R>::ConnectFunc connect);

  std::shared_ptr<Alarm> CreateAlarm();
};

}
```

サーバ側:

```cpp
namespace ggrpc {

// 単体リクエスト用
enum class ServerResponseWriterError {
  WRITE,
};

template <class W, class R>
class ServerResponseWriterContext {
 public:
  Server* GetServer() const;
  void Finish(W resp, grpc::Status status);
  void FinishWithError(grpc::Status status);
  void Close();
};

template <class W, class R>
class ServerResponseWriterHandler {
 public:
  std::shared_ptr<ServerResponseWriterContext<W, R>> Context() const;

  virtual void Request(grpc::ServerContext* context, R* request,
                       grpc::ServerAsyncResponseWriter<W>* response_writer,
                       grpc::ServerCompletionQueue* cq, void* tag) = 0;
  virtual void OnAccept(R request) {}
  virtual void OnFinish(W response, grpc::Status status) {}
  virtual void OnError(ServerResponseWriterError error) {}
};

// 双方向ストリーミング用
enum class ServerReaderWriterError {
  WRITE,
};

template <class W, class R>
class ServerReaderWriterContext {
 public:
  Server* GetServer() const;
  void Write(W resp, int64_t id);
  void Finish(grpc::Status status);
  void Close();
};

template <class W, class R>
class ServerReaderWriterHandler {
 public:
  std::shared_ptr<ServerReaderWriterContext<W, R>> Context() const;

  virtual void Request(grpc::ServerContext* context,
                       grpc::ServerAsyncReaderWriter<W, R>* streamer,
                       grpc::ServerCompletionQueue* cq, void* tag) = 0;
  virtual void OnAccept() {}
  virtual void OnRead(R req) {}
  virtual void OnReadDoneOrError() {}
  virtual void OnWrite(W response, int64_t id) {}
  virtual void OnFinish(grpc::Status status) {}
  virtual void OnError(ServerReaderWriterError error) {}
};

class Server {
 public:
  template <class H, class... Args>
  void AddResponseWriterHandler(Args... args);

  template <class H, class... Args>
  void AddReaderWriterHandler(Args... args);

  void Start(grpc::ServerBuilder& builder, int threads);
  void Wait();
  void Shutdown();

  std::shared_ptr<Alarm> CreateAlarm();
};

}
```

共通:

```cpp
namespace ggrpc {

class Alarm {
 public:
  typedef std::function<void(bool)> AlarmFunc;

  bool Set(std::chrono::milliseconds deadline, AlarmFunc alarm);

  void Cancel();
  void Close();
};

}
```

## TODO

- ドキュメント書く
- クライアントストリーミング、サーバストリーミングを実装する
- spdlog 依存を無くす
- pubsub 的なサンプルを書く
