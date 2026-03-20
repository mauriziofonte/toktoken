import gleam/http
import gleam/result

pub const default_port = 8080

pub const max_connections = 1000

pub type Request {
  Request(method: String, path: String, body: String, headers: List(#(String, String)))
}

pub type Response {
  Response(status: Int, body: String, headers: List(#(String, String)))
}

pub fn start(port: Int) -> Result(Nil, String) {
  case port > 0 && port < 65536 {
    True -> {
      let config = configure_server(port)
      case config {
        Ok(_) -> Ok(Nil)
        Error(msg) -> Error(msg)
      }
    }
    False -> Error("Invalid port number")
  }
}

fn configure_server(port: Int) -> Result(Int, String) {
  case port {
    p if p < 1024 -> Error("Port requires elevated privileges")
    _ -> Ok(port)
  }
}

pub fn handle_request(req: Request) -> Response {
  case req.method {
    "GET" -> handle_get(req)
    "POST" -> handle_post(req)
    _ -> Response(status: 405, body: "Method not allowed", headers: [])
  }
}

fn handle_get(req: Request) -> Response {
  case req.path {
    "/" -> Response(status: 200, body: "Welcome", headers: [])
    "/health" -> Response(status: 200, body: "OK", headers: [])
    _ -> Response(status: 404, body: "Not found", headers: [])
  }
}

fn handle_post(req: Request) -> Response {
  case req.body {
    "" -> Response(status: 400, body: "Empty body", headers: [])
    body -> Response(status: 201, body: body, headers: [#("content-type", "application/json")])
  }
}
