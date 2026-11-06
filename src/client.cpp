#include "matchmaker/client.hpp"

#include "util.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <stdexcept>

namespace matchmaker
{
namespace
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

http::verb to_verb(const std::string& method)
{
  if( method == "GET" )
    return http::verb::get;
  if( method == "POST" )
    return http::verb::post;
  if( method == "DELETE" )
    return http::verb::delete_;
  if( method == "PUT" )
    return http::verb::put;
  if( method == "PATCH" )
    return http::verb::patch;
  throw std::runtime_error("unsupported method");
}

} // namespace

struct Client::Impl
{
  ClientOptions options;
  boost::asio::io_context ioc;
  ssl::context ctx{ssl::context::tls_client};
  std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws;
  beast::flat_buffer ws_buffer;
  std::string offered;
  bool open = false;

  explicit Impl(ClientOptions opts) : options(std::move(opts))
  {
    ctx.set_verify_mode(options.insecure ? ssl::verify_none : ssl::verify_peer);
  }
};

Client::Client(ClientOptions options) : impl_(std::make_unique<Impl>(std::move(options)))
{
}

Client::Client(Client&&) noexcept = default;

Client& Client::operator=(Client&&) noexcept = default;

Client::~Client()
{
  try
  {
    ws_close();
  }
  catch( ... )
  {
  }
}

HttpResult Client::request(const std::string& method, const std::string& target,
                           const nlohmann::json& body, const std::string& bearer, bool accept_gzip)
{
  HttpResult result;
  try
  {
    impl_->ioc.restart();
    tcp::resolver resolver(impl_->ioc);
    beast::ssl_stream<beast::tcp_stream> stream(impl_->ioc, impl_->ctx);
    const auto endpoints =
        resolver.resolve(impl_->options.host, std::to_string(impl_->options.port));
    beast::get_lowest_layer(stream).expires_after(impl_->options.timeout);
    beast::get_lowest_layer(stream).connect(endpoints);
    if( !SSL_set_tlsext_host_name(stream.native_handle(), impl_->options.host.c_str()) )
    {
      throw std::runtime_error("SNI failed");
    }
    stream.handshake(ssl::stream_base::client);
    http::request<http::string_body> req{to_verb(method), target, 11};
    req.set(http::field::host, impl_->options.host);
    req.set(http::field::user_agent, "matchmaker-client");
    req.set(http::field::accept, "application/json");
    if( accept_gzip )
    {
      req.set(http::field::accept_encoding, "gzip");
    }
    if( !bearer.empty() )
    {
      req.set(http::field::authorization, "Bearer " + bearer);
    }
    if( method != "GET" && method != "DELETE" )
    {
      req.set(http::field::content_type, "application/json");
      req.body() = body.dump();
    }
    req.prepare_payload();
    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    result.status = static_cast<int>(res.result_int());
    result.content_encoding = std::string(res[http::field::content_encoding]);
    result.raw_body = res.body();
    std::string text = res.body();
    if( result.content_encoding.find("gzip") != std::string::npos )
    {
      auto decoded = gzip_decompress(text);
      if( !decoded )
      {
        result.error = "gzip decode failed";
        return result;
      }
      text = *decoded;
    }
    if( !text.empty() )
    {
      auto parsed = nlohmann::json::parse(text, nullptr, false);
      if( parsed.is_discarded() )
      {
        result.error = "invalid json";
        result.body = text;
      }
      else
      {
        result.body = std::move(parsed);
      }
    }
    beast::error_code ec;
    stream.shutdown(ec);
  }
  catch( const std::exception& ex )
  {
    result.error = ex.what();
  }
  return result;
}

void Client::ws_connect(const std::string& lobby_id, const std::string& bearer)
{
  ws_close();
  impl_->ioc.restart();
  tcp::resolver resolver(impl_->ioc);
  auto stream = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
      impl_->ioc, impl_->ctx);
  const auto endpoints = resolver.resolve(impl_->options.host, std::to_string(impl_->options.port));
  beast::get_lowest_layer(*stream).expires_after(impl_->options.timeout);
  beast::get_lowest_layer(*stream).connect(endpoints);
  if( !SSL_set_tlsext_host_name(stream->next_layer().native_handle(), impl_->options.host.c_str()) )
  {
    throw std::runtime_error("SNI failed");
  }
  stream->next_layer().handshake(ssl::stream_base::client);
  websocket::permessage_deflate deflate;
  deflate.client_enable = true;
  deflate.server_enable = false;
  deflate.client_no_context_takeover = true;
  deflate.server_no_context_takeover = true;
  deflate.client_max_window_bits = 12;
  deflate.compLevel = 3;
  stream->set_option(deflate);
  impl_->offered.clear();
  stream->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
  const std::string target = "/ws/v1/lobbies/" + lobby_id;
  stream->set_option(websocket::stream_base::decorator(
      [this, bearer](websocket::request_type& req)
      {
        req.set(http::field::authorization, "Bearer " + bearer);
        impl_->offered = std::string(req[http::field::sec_websocket_extensions]);
      }));
  stream->handshake(impl_->options.host, target);
  beast::get_lowest_layer(*stream).expires_never();
  impl_->ws = std::move(stream);
  impl_->open = true;
}

void Client::ws_send(const nlohmann::json& message)
{
  if( !impl_->ws )
  {
    throw std::runtime_error("websocket is not open");
  }
  impl_->ws->text(true);
  impl_->ws->write(boost::asio::buffer(message.dump()));
}

nlohmann::json Client::ws_read(std::chrono::milliseconds timeout)
{
  if( !impl_->ws )
  {
    throw std::runtime_error("websocket is not open");
  }
  impl_->ioc.restart();
  beast::get_lowest_layer(*impl_->ws).expires_after(timeout);
  impl_->ws_buffer.clear();
  impl_->ws->read(impl_->ws_buffer);
  const auto text = beast::buffers_to_string(impl_->ws_buffer.data());
  auto parsed = nlohmann::json::parse(text, nullptr, false);
  if( parsed.is_discarded() )
  {
    throw std::runtime_error("websocket payload is not json");
  }
  return parsed;
}

void Client::ws_close()
{
  if( !impl_->ws )
  {
    impl_->open = false;
    return;
  }
  beast::error_code ec;
  if( impl_->open )
  {
    impl_->ws->close(websocket::close_code::normal, ec);
  }
  impl_->ws.reset();
  impl_->open = false;
}

std::string Client::ws_offered_extensions() const
{
  return impl_->offered;
}

bool Client::ws_open() const
{
  return impl_->open && impl_->ws != nullptr;
}

} // namespace matchmaker
