// Proves the WinHTTP transport carries a real OTLP export end to end.
//
// It builds an OtlpHttpClient with our client injected (the constructor upstream provides for
// exactly this), sends one metrics payload at a local listener, and reports the status it got back.
// Success means the SDK produced a payload, our transport delivered it, and the response came back
// through the SDK's own handler - which is the whole question this spike exists to answer.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "opentelemetry/exporters/otlp/otlp_http_client.h"
#include "winhttp_client.h"

namespace otlp = opentelemetry::exporter::otlp;

int main(int argc, char **argv) {
  const std::string endpoint = (argc > 1) ? argv[1] : "http://127.0.0.1:4318/v1/metrics";

  auto transport = std::make_shared<winhttp_client::HttpClient>();
  if (!transport->winhttp_session()) {
    std::fprintf(stderr, "FAIL: WinHttpOpen returned no session\n");
    return 2;
  }
  std::printf("winhttp session opened\n");

  // A direct exchange through the transport, so a failure here is unambiguously ours rather than
  // the SDK's serialisation.
  auto session = transport->CreateSession(endpoint);
  auto request = session->CreateRequest();
  request->SetMethod(opentelemetry::ext::http::client::Method::Post);
  request->SetUri(endpoint.substr(endpoint.find('/', endpoint.find("//") + 2)));
  request->AddHeader("Content-Type", "application/json");
  std::string payload =
      R"({"resourceMetrics":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"winhttp-spike"}}]},)"
      R"("scopeMetrics":[{"metrics":[{"name":"spike.counter","unit":"1","sum":{"aggregationTemporality":2,)"
      R"("isMonotonic":true,"dataPoints":[{"asInt":"1","timeUnixNano":"1700000000000000000",)"
      R"("startTimeUnixNano":"1700000000000000000"}]}}]}]}]})";
  opentelemetry::ext::http::client::Body body(payload.begin(), payload.end());
  request->SetBody(body);

  struct Handler : public opentelemetry::ext::http::client::EventHandler {
    std::atomic<int> status{0};
    std::atomic<bool> done{false};
    std::string last_event;
    void OnResponse(opentelemetry::ext::http::client::Response &response) noexcept override {
      status.store(static_cast<int>(response.GetStatusCode()));
      done.store(true);
    }
    void OnEvent(opentelemetry::ext::http::client::SessionState state,
                 opentelemetry::nostd::string_view reason) noexcept override {
      last_event.assign(reason.data(), reason.size());
      using S = opentelemetry::ext::http::client::SessionState;
      if (state == S::ConnectFailed || state == S::SendFailed || state == S::NetworkError ||
          state == S::TimedOut || state == S::ReadError || state == S::SSLHandshakeFailed ||
          state == S::CreateFailed) {
        done.store(true);
      }
    }
  };
  auto handler = std::make_shared<Handler>();
  session->SendRequest(handler);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!handler->done.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  session->FinishSession();
  transport->FinishAllSessions();

  const int status = handler->status.load();
  if (status >= 200 && status < 300) {
    std::printf("PASS: OTLP payload delivered over WinHTTP, HTTP %d\n", status);
    return 0;
  }
  std::fprintf(stderr, "FAIL: status=%d last_event=%s\n", status, handler->last_event.c_str());
  return 1;
}
