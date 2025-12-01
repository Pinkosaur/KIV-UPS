import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

/**
 * NetworkClient
 *
 * Minimal networking wrapper that:
 *  - connects to a host:port
 *  - sends an initial "HELLO <name>" line after connecting
 *  - starts a reader thread that reads lines and forwards them to the provided listener
 *  - exposes send(String) to write lines to the server
 *  - provides an explicit close() to stop the reader and close resources
 *
 * It intentionally does NOT parse server messages; it only forwards raw lines to the listener
 * so the GUI can keep the exact original parsing logic.
 */
public class NetworkClient {
    public interface NetworkListener {
        /** Called once the socket is connected and streams are ready. */
        void onConnected();
        /** Called when the socket is closed / EOF reached. */
        void onDisconnected();
        /** Called for each server-sent line (without trailing newline). */
        void onServerMessage(String line);
        /** Called when an I/O error occurs while connecting/reading/sending. */
        void onNetworkError(Exception ex);
    }

    private final NetworkListener listener;

    private Socket socket;
    private BufferedReader in;
    private PrintWriter out;
    private Thread readerThread;

    private volatile boolean closed = false;
    private volatile boolean connected = false;

    public NetworkClient(NetworkListener listener) {
        this.listener = listener;
    }

    /**
     * Connect to the server and start the background reader thread.
     * If connection fails, listener.onNetworkError() will be invoked.
     *
     * This method returns quickly; the reader runs in a background daemon thread.
     *
     * After connecting, this will send: "HELLO " + clientName as the first line to server
     * (to match the behavior in the original client).
     */
    public synchronized void connect(String host, int port, String clientName) {
        if (connected) return;
        closed = false;

        try {
            socket = new Socket(host, port);
            in = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
            out = new PrintWriter(new OutputStreamWriter(socket.getOutputStream(), StandardCharsets.UTF_8), true);
            connected = true;
            // notify listener that we're connected (on caller thread)
            try {
                listener.onConnected();
            } catch (Exception ex) {
                // listener may throw — don't let that prevent networking
                ex.printStackTrace();
            }
            // send hello immediately (matches original client behavior)
            if (clientName != null) {
                sendRaw("HELLO " + clientName);
            }

            // start reader thread
            readerThread = new Thread(this::readLoop, "NetworkClient-Reader");
            readerThread.setDaemon(true);
            readerThread.start();
        } catch (IOException ex) {
            // cleanup partial state
            safeCloseInternal();
            connected = false;
            try { listener.onNetworkError(ex); } catch (Exception ignore) {}
        }
    }

    /** Return true if currently connected and not closed. */
    public boolean isConnected() { return connected && !closed && socket != null && socket.isConnected() && !socket.isClosed(); }

    /** Send a line to the server (writes the string and a newline). Thread-safe. */
    public synchronized void sendRaw(String line) {
        if (out == null || closed) return;
        try {
            out.println(line);
            out.flush();
        } catch (Exception ex) {
            // If sending fails, notify listener and attempt to close.
            try { listener.onNetworkError(ex); } catch (Exception ignore) {}
            safeCloseInternal();
        }
    }

    /** Convenience helper for the MOVE command used by the GUI */
    public void sendMove(String move) {
        if (move == null) return;
        sendRaw("MOVE " + move);
    }

    /** Close connection and stop reader thread (idempotent). */
    public synchronized void closeConnection() {
        if (closed) return;
        closed = true;
        safeCloseInternal();
    }

    /* INTERNAL ****************************************/

    private void readLoop() {
        try {
            String line;
            while (!closed && (line = in.readLine()) != null) {
                // forward raw line to listener; listener is responsible for Swing thread marshaling
                try {
                    listener.onServerMessage(line);
                } catch (Exception ex) {
                    // Listener threw — report but continue reading
                    ex.printStackTrace();
                }
            }
            // EOF reached (peer closed)
            try {
                listener.onDisconnected();
            } catch (Exception ex) {
                ex.printStackTrace();
            }
        } catch (IOException ex) {
            if (!closed) {
                try { listener.onNetworkError(ex); } catch (Exception ignore) {}
            }
        } finally {
            safeCloseInternal();
        }
    }

    /** Close socket/streams without throwing. */
    private synchronized void safeCloseInternal() {
        connected = false;
        try {
            if (in != null) {
                try { in.close(); } catch (Exception ignored) {}
                in = null;
            }
        } catch (Throwable ignored) {}
        try {
            if (out != null) {
                try { out.close(); } catch (Exception ignored) {}
                out = null;
            }
        } catch (Throwable ignored) {}
        try {
            if (socket != null) {
                try { socket.close(); } catch (Exception ignored) {}
                socket = null;
            }
        } catch (Throwable ignored) {}
        try {
            if (readerThread != null) {
                readerThread.interrupt();
                readerThread = null;
            }
        } catch (Throwable ignored) {}
    }
}
