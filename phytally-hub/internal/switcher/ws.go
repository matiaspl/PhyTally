package switcher

import (
	"bufio"
	"context"
	"crypto/rand"
	"crypto/sha1"
	"crypto/tls"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/textproto"
	"net/url"
	"strings"
	"time"
)

const websocketGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

type wsConn struct {
	conn   net.Conn
	reader *bufio.Reader
}

func dialWebsocket(ctx context.Context, endpoint *url.URL) (*wsConn, error) {
	scheme := strings.ToLower(endpoint.Scheme)
	hostPort := endpoint.Host
	if !strings.Contains(hostPort, ":") {
		switch scheme {
		case "wss":
			hostPort += ":443"
		default:
			hostPort += ":80"
		}
	}

	dialer := net.Dialer{Timeout: 5 * time.Second}
	var conn net.Conn
	var err error
	switch scheme {
	case "wss":
		conn, err = tls.DialWithDialer(&dialer, "tcp", hostPort, &tls.Config{
			ServerName: strings.Split(endpoint.Host, ":")[0],
		})
	case "ws":
		conn, err = dialer.DialContext(ctx, "tcp", hostPort)
	default:
		return nil, fmt.Errorf("unsupported OBS websocket scheme %q", endpoint.Scheme)
	}
	if err != nil {
		return nil, err
	}

	reader := bufio.NewReader(conn)
	keyBytes := make([]byte, 16)
	if _, err := rand.Read(keyBytes); err != nil {
		conn.Close()
		return nil, err
	}
	key := base64.StdEncoding.EncodeToString(keyBytes)
	path := endpoint.RequestURI()
	if path == "" {
		path = "/"
	}

	request := fmt.Sprintf(
		"GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n\r\n",
		path,
		endpoint.Host,
		key,
	)
	if _, err := io.WriteString(conn, request); err != nil {
		conn.Close()
		return nil, err
	}

	tp := textproto.NewReader(reader)
	statusLine, err := tp.ReadLine()
	if err != nil {
		conn.Close()
		return nil, err
	}
	if !strings.Contains(statusLine, "101") {
		conn.Close()
		return nil, fmt.Errorf("websocket upgrade failed: %s", statusLine)
	}

	mimeHeader, err := tp.ReadMIMEHeader()
	if err != nil {
		conn.Close()
		return nil, err
	}
	if !strings.EqualFold(mimeHeader.Get("Upgrade"), "websocket") {
		conn.Close()
		return nil, fmt.Errorf("invalid websocket upgrade response")
	}

	expectedAccept := websocketAccept(key)
	if mimeHeader.Get("Sec-WebSocket-Accept") != expectedAccept {
		conn.Close()
		return nil, fmt.Errorf("invalid websocket accept header")
	}

	return &wsConn{conn: conn, reader: reader}, nil
}

func (w *wsConn) Close() {
	_ = w.conn.Close()
}

func (w *wsConn) writeJSON(payload any) error {
	data, err := json.Marshal(payload)
	if err != nil {
		return err
	}
	return w.writeFrame(0x1, data)
}

func (w *wsConn) readJSON(target any) error {
	for {
		payload, opcode, err := w.readFrame()
		if err != nil {
			return err
		}
		switch opcode {
		case 0x1:
			return json.Unmarshal(payload, target)
		case 0x9:
			if err := w.writeFrame(0xA, payload); err != nil {
				return err
			}
		case 0x8:
			return io.EOF
		}
	}
}

func (w *wsConn) writeFrame(opcode byte, payload []byte) error {
	if err := w.conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return err
	}

	header := []byte{0x80 | opcode}
	payloadLen := len(payload)
	switch {
	case payloadLen < 126:
		header = append(header, byte(0x80|payloadLen))
	case payloadLen <= 0xffff:
		header = append(header, 0x80|126)
		ext := make([]byte, 2)
		binary.BigEndian.PutUint16(ext, uint16(payloadLen))
		header = append(header, ext...)
	default:
		header = append(header, 0x80|127)
		ext := make([]byte, 8)
		binary.BigEndian.PutUint64(ext, uint64(payloadLen))
		header = append(header, ext...)
	}

	mask := make([]byte, 4)
	if _, err := rand.Read(mask); err != nil {
		return err
	}
	header = append(header, mask...)

	masked := make([]byte, len(payload))
	for index := range payload {
		masked[index] = payload[index] ^ mask[index%4]
	}

	if _, err := w.conn.Write(header); err != nil {
		return err
	}
	_, err := w.conn.Write(masked)
	return err
}

func (w *wsConn) readFrame() ([]byte, byte, error) {
	if err := w.conn.SetReadDeadline(time.Now().Add(10 * time.Second)); err != nil {
		return nil, 0, err
	}

	header := make([]byte, 2)
	if _, err := io.ReadFull(w.reader, header); err != nil {
		return nil, 0, err
	}

	opcode := header[0] & 0x0f
	payloadLen := int(header[1] & 0x7f)
	masked := header[1]&0x80 != 0

	switch payloadLen {
	case 126:
		ext := make([]byte, 2)
		if _, err := io.ReadFull(w.reader, ext); err != nil {
			return nil, 0, err
		}
		payloadLen = int(binary.BigEndian.Uint16(ext))
	case 127:
		ext := make([]byte, 8)
		if _, err := io.ReadFull(w.reader, ext); err != nil {
			return nil, 0, err
		}
		payloadLen = int(binary.BigEndian.Uint64(ext))
	}

	mask := make([]byte, 4)
	if masked {
		if _, err := io.ReadFull(w.reader, mask); err != nil {
			return nil, 0, err
		}
	}

	payload := make([]byte, payloadLen)
	if _, err := io.ReadFull(w.reader, payload); err != nil {
		return nil, 0, err
	}
	if masked {
		for index := range payload {
			payload[index] ^= mask[index%4]
		}
	}
	return payload, opcode, nil
}

func websocketAccept(key string) string {
	sum := sha1.Sum([]byte(key + websocketGUID))
	return base64.StdEncoding.EncodeToString(sum[:])
}

var _ = http.Header{}
