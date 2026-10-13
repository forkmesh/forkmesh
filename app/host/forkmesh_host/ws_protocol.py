from __future__ import annotations

from uvicorn.protocols.websockets.websockets_sansio_impl import (
    WebSocketsSansIOProtocol,
)

SHUTDOWN_STATUS = 503
SHUTDOWN_BODY = "Service Unavailable\n"
SHUTDOWN_RETRY_AFTER_SECONDS = 5
EXPECTED_DEGRADED_HEADER = "X-Forkmesh-Expected-Degraded"


class DrainingWebSocketProtocol(WebSocketsSansIOProtocol):
    def shutdown(self) -> None:
        if self.close_sent or self.handshake_complete \
                or self.initial_response is not None:
            super().shutdown()
            return
        self.stop_keepalive()
        response = self.conn.reject(SHUTDOWN_STATUS, SHUTDOWN_BODY)
        response.headers["Retry-After"] = str(SHUTDOWN_RETRY_AFTER_SECONDS)
        response.headers[EXPECTED_DEGRADED_HEADER] = "1"
        self.conn.send_response(response)
        self.transport.write(b"".join(self.conn.data_to_send()))
        self.transport.close()


def stopping_protocol(base, stopping):
    class StoppingProtocol(base):
        def shutdown(self) -> None:
            stopping.set()
            super().shutdown()

    StoppingProtocol.__name__ = "Stopping" + base.__name__
    StoppingProtocol.__qualname__ = StoppingProtocol.__name__
    return StoppingProtocol
