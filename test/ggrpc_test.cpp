#include <functional>
#include <iostream>
#include <memory>
#include <thread>

// ggrpc
#include <ggrpc/client.h>
#include <ggrpc/server.h>

// spdlog
#include <spdlog/spdlog.h>

#include "ggrpc.grpc.pb.h"
#include "ggrpc.pb.h"

class ClientManager {
  std::unique_ptr<ggrpc::ClientManager> gcm_;
  std::unique_ptr<ggrpctest::Test::Stub> stub_;

 public:
  ClientManager(std::shared_ptr<grpc::Channel> channel, int threads)
      : stub_(ggrpctest::Test::NewStub(channel)),
        gcm_(new ggrpc::ClientManager(threads)) {}

  void Shutdown() { gcm_->Shutdown(); }

  std::unique_ptr<ggrpc::WritableClient<ggrpctest::BidiRequest>> TestBidi(
      std::function<void(ggrpctest::BidiResponse)> read,
      std::function<void(grpc::Status)> done,
      std::function<void(ggrpc::ClientReaderWriterError)> on_error) {
    auto async_connect = [stub = stub_.get()](grpc::ClientContext* context,
                                              grpc::CompletionQueue* cq,
                                              void* tag) {
      return stub->AsyncBidi(context, cq, tag);
    };
    return gcm_->CreateReaderWriterClient<ggrpctest::BidiRequest,
                                          ggrpctest::BidiResponse>(
        std::move(async_connect), std::move(read), std::move(done),
        std::move(on_error));
  }

  std::unique_ptr<ggrpc::Client> TestUnary(
      ggrpctest::UnaryRequest request,
      std::function<void(ggrpctest::UnaryResponse, grpc::Status)> done,
      std::function<void(ggrpc::ClientResponseReaderError)> on_error) {
    auto async_connect = [stub = stub_.get()](
                             grpc::ClientContext* context,
                             const ggrpctest::UnaryRequest& request,
                             grpc::CompletionQueue* cq) {
      return stub->AsyncUnary(context, request, cq);
    };
    return gcm_
        ->CreateClient<ggrpctest::UnaryRequest, ggrpctest::UnaryResponse>(
            std::move(async_connect), std::move(request), std::move(done),
            std::move(on_error));
  }

  std::unique_ptr<ggrpc::Client> CreateAlarm(std::chrono::milliseconds ms,
                                             std::function<void()> f) {
    return gcm_->CreateAlarm(ms, std::move(f));
  }
};

class TestUnaryHandler
    : public ggrpc::ServerResponseWriterHandler<ggrpctest::UnaryResponse,
                                                ggrpctest::UnaryRequest> {
  ggrpctest::Test::AsyncService* service_;

 public:
  TestUnaryHandler(ggrpctest::Test::AsyncService* service)
      : service_(service) {}
  void OnRequest(
      grpc::ServerContext* context, ggrpctest::UnaryRequest* request,
      grpc::ServerAsyncResponseWriter<ggrpctest::UnaryResponse>* response,
      grpc::ServerCompletionQueue* cq, void* tag) override {
    service_->RequestUnary(context, request, response, cq, cq, tag);
  }
  void OnAccept(ggrpctest::UnaryRequest request) override {
    SPDLOG_TRACE("received UnaryRequest: {}", request.DebugString());
    ggrpctest::UnaryResponse resp;
    resp.set_value(request.value() * 100);
    GetContext()->Finish(resp, grpc::Status::OK);
  }
};

class TestBidiHandler
    : public ggrpc::ServerReaderWriterHandler<ggrpctest::BidiResponse,
                                              ggrpctest::BidiRequest> {
  ggrpctest::Test::AsyncService* service_;

 public:
  TestBidiHandler(ggrpctest::Test::AsyncService* service) : service_(service) {}
  void OnRequest(grpc::ServerContext* context,
                 grpc::ServerAsyncReaderWriter<
                     ggrpctest::BidiResponse, ggrpctest::BidiRequest>* streamer,
                 grpc::ServerCompletionQueue* cq, void* tag) override {
    service_->RequestBidi(context, streamer, cq, cq, tag);
  }
  void OnAccept() override {
    ggrpctest::BidiResponse resp;
    resp.set_value(1);
    GetContext()->Write(resp);
  }
  void OnRead(ggrpctest::BidiRequest req) override {
    SPDLOG_TRACE("received BidiRequest {}", req.DebugString());
    ggrpctest::BidiResponse resp;
    resp.set_value(2);
    GetContext()->Write(resp);
  }
  void OnReadDoneOrError() override {
    ggrpctest::BidiResponse resp;
    resp.set_value(3);
    GetContext()->Write(resp);
    GetContext()->Finish(grpc::Status::OK);
  }
};

class TestServer {
  ggrpctest::Test::AsyncService service_;
  std::unique_ptr<ggrpc::Server> server_;

 public:
  void Start(std::string address, int threads) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);

    std::vector<std::unique_ptr<grpc::ServerCompletionQueue>> cqs;
    for (int i = 0; i < threads; i++) {
      cqs.push_back(builder.AddCompletionQueue());
    }

    server_ = std::unique_ptr<ggrpc::Server>(
        new ggrpc::Server(builder.BuildAndStart(), std::move(cqs)));

    SPDLOG_INFO("gRPC Server listening on {}", address);

    // ハンドラの登録
    server_->AddReaderWriterHandler<TestBidiHandler>(&service_);
    server_->AddResponseWriterHandler<TestUnaryHandler>(&service_);

    server_->Start();
  }
};

int main() {
  spdlog::set_level(spdlog::level::trace);

  std::unique_ptr<TestServer> server(new TestServer());
  server->Start("0.0.0.0:50051", 10);
  std::this_thread::sleep_for(std::chrono::seconds(1));

  auto channel = grpc::CreateChannel("localhost:50051",
                                     grpc::InsecureChannelCredentials());
  std::unique_ptr<ClientManager> cm(new ClientManager(channel, 10));
  auto bidi = cm->TestBidi(
      [](ggrpctest::BidiResponse resp) {
        SPDLOG_TRACE("received BidiResponse {}", resp.DebugString());
      },
      [](grpc::Status status) {
        SPDLOG_TRACE("gRPC Error: {}", status.error_message());
      },
      [](ggrpc::ClientReaderWriterError error) {
        SPDLOG_TRACE("ClientReaderWriterError: {}", (int)error);
      });
  std::this_thread::sleep_for(std::chrono::seconds(1));
  ggrpctest::BidiRequest req;
  req.set_value(100);
  bidi->Write(req);
  bidi->WritesDone();
  std::this_thread::sleep_for(std::chrono::seconds(1));

  auto alarm = cm->CreateAlarm(std::chrono::milliseconds(100),
                               []() { SPDLOG_TRACE("Alarm"); });
  std::this_thread::sleep_for(std::chrono::seconds(1));

  alarm = cm->CreateAlarm(std::chrono::milliseconds(100), [&]() {
    SPDLOG_TRACE("Alarm 2");
    alarm = cm->CreateAlarm(std::chrono::milliseconds(100),
                            []() { SPDLOG_TRACE("Alarm 3"); });
  });
  std::this_thread::sleep_for(std::chrono::seconds(1));

  // 即座にキャンセルする
  alarm = cm->CreateAlarm(std::chrono::milliseconds(100),
                          [&]() { SPDLOG_ERROR("Alarm 4"); });
  alarm.reset();
  std::this_thread::sleep_for(std::chrono::seconds(1));

  ggrpctest::UnaryRequest unary_req;
  unary_req.set_value(1);
  auto unary = cm->TestUnary(
      unary_req,
      [](ggrpctest::UnaryResponse resp, grpc::Status status) {
        SPDLOG_TRACE("received UnaryResponse {}", resp.DebugString());
      },
      [](ggrpc::ClientResponseReaderError error) {
        SPDLOG_TRACE("ClientResponseReaderError: {}", (int)error);
      });
  std::this_thread::sleep_for(std::chrono::seconds(1));
}
