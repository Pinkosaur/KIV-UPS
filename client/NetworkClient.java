import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.time.LocalTime;
import java.time.format.DateTimeFormatter;

/**
 * NetworkClient
 *
 * Handles low-level TCP socket communication for the chess application.
 * Manages separate threads for reading, writing, and heartbeats to ensure
 * a responsive UI. Communication follows a line-based text protocol with
 * an automated acknowledgement handshake layer.
 */
public class NetworkClient {
    /**
     * Listener interface for receiving network events and server messages.
     */
    public interface NetworkListener {
        /**
         * Invoked when the TCP connection is successfully established.
         */
        void onConnected();

        /**
         * Invoked when the connection is lost or closed.
         */
        void onDisconnected();

        /**
         * Invoked when a complete protocol message is received from the server.
         * @param line The raw message payload without sequencing suffixes.
         */
        void onServerMessage(String line);

        /**
         * Invoked when a networking exception occurs.
         * @param ex The exception encountered.
         */
        void onNetworkError(Exception ex);
    }

    private final NetworkListener listener;
    private Socket socket;
    private BufferedReader in;
    private PrintWriter out;
    
    private Thread readerThread;
    private Thread heartbeatThread;
    private Thread writerThread;
    private final BlockingQueue<String> writeQueue = new LinkedBlockingQueue<>();
    
    private volatile long lastRxTime = 0;
    private static final long HEARTBEAT_INTERVAL = 2000; 
    private static final long READ_TIMEOUT = 10000;      

    private volatile boolean closed = false;
    private volatile boolean connected = false;

    /**
     * Constructs a new NetworkClient.
     * @param listener The callback implementation for network events.
     */
    public NetworkClient(NetworkListener listener) {
        this.listener = listener;
    }

    /**
     * Initiates a connection to the specified server and performs the initial handshake.
     * Starts background threads for protocol handling upon success.
     *
     * @param host Server hostname or IP.
     * @param port Server port number.
     * @param clientName The display name for the user.
     * @param sessionID Unique identifier used for session persistence.
     * @throws IOException If the connection cannot be established.
     */
    public void connect(String host, int port, String clientName, String sessionID) throws IOException {
        socket = new Socket(host, port);
        socket.setSoTimeout(0); 
        
        in = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
        out = new PrintWriter(new OutputStreamWriter(socket.getOutputStream(), StandardCharsets.UTF_8), true);
        
        connected = true;
        closed = false;
        lastRxTime = System.currentTimeMillis();
        
        if (listener != null) listener.onConnected();
        
        // Initiate protocol handshake immediately upon connection
        sendRaw("HELLO " + clientName + " " + sessionID);
        
        startThreads();
    }

    /**
     * Initializes and starts background processing threads for the socket.
     */
    private void startThreads() {
        readerThread = new Thread(this::readLoop, "NetworkReader");
        writerThread = new Thread(this::writeLoop, "NetworkWriter");
        heartbeatThread = new Thread(this::heartbeatLoop, "NetworkHeartbeat");
        
        readerThread.start();
        writerThread.start();
        heartbeatThread.start();
    }

    /**
     * Continuously reads lines from the socket input stream.
     * Handles ACK filtering: Protocol ACKs update the heartbeat but are not propagated
     * to the application listener. Standard commands are ACKed and passed up.
     */
    private void readLoop() {
        try {
            String line;
            while (connected && !closed && (line = in.readLine()) != null) {
                lastRxTime = System.currentTimeMillis();
                
                // Handle Heartbeat PONG
                if (line.equals(Protocol.RESP_PING)) continue; 
                
                // Handle Server Acknowledgements (2-digit numeric codes)
                // We validate them here (print to console) but do not pass to UI.
                if (line.length() == 2 && Character.isDigit(line.charAt(0)) && Character.isDigit(line.charAt(1))) {
                    System.out.println("ACK RX: " + line);
                    continue; 
                }

                // Handle Standard Protocol Commands
                final String payload = line;
                
                // Send automated acknowledgement for the received command
                String ack = Protocol.getAckFor(payload);
                if (ack != null) sendRaw(ack);

                try {
                    if (listener != null) listener.onServerMessage(payload);
                } catch (Exception ex) { 
                    ex.printStackTrace(); 
                }
            }
        } catch (IOException e) {
            if (connected && !closed && listener != null) {
                listener.onNetworkError(e);
            }
        } finally {
            safeCloseInternal();
            if (listener != null) listener.onDisconnected();
        }
    }

    /**
     * Processes the outbound message queue and writes lines to the socket.
     */
    private void writeLoop() {
        try {
            while (connected && !closed) {
                String msg = writeQueue.take();
                out.println(msg);
            }
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        } catch (Exception e) {
            log("ERROR", "Writer thread error: " + e.getMessage());
        }
    }

    /**
     * Monitors connection health by periodically sending PINGs and checking for read timeouts.
     */
    private void heartbeatLoop() {
        try {
            while (connected && !closed) {
                Thread.sleep(HEARTBEAT_INTERVAL);
                
                if (System.currentTimeMillis() - lastRxTime > READ_TIMEOUT) {
                    log("WARN", "Read timeout detected. Closing connection.");
                    break;
                }
                sendRaw(Protocol.CMD_PING);
            }
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        } finally {
            safeCloseInternal();
        }
    }

    /**
     * Helper for formatted console logging with timestamps.
     */
    private void log(String level, String msg) {
        String time = LocalTime.now().format(DateTimeFormatter.ofPattern("HH:mm:ss"));
        System.out.println(String.format("[%s] [%s] %s", time, level, msg));
    }
    
    /**
     * Queues a raw string for transmission to the server.
     * @param msg Protocol message line.
     */
    public void sendRaw(String msg) {
        if (!connected || closed) return;
        writeQueue.offer(msg);
        if (!msg.contains(Protocol.CMD_PING)) System.out.println(">> " + msg);
    }

    /**
     * Explicitly closes the network connection and shuts down all active background threads.
     */
    public synchronized void closeConnection() {
        if (closed) return;
        log("INFO", "Closing network connection.");
        closed = true;
        connected = false;
        
        if (heartbeatThread != null) heartbeatThread.interrupt();
        if (readerThread != null) readerThread.interrupt();
        if (writerThread != null) writerThread.interrupt();
        
        try { if (in != null) in.close(); } catch (Exception e) {}
        try { if (out != null) out.close(); } catch (Exception e) {}
        try { if (socket != null) socket.close(); } catch (Exception e) {}
        
        writeQueue.clear();
    }
    
    /**
     * Returns the current connection status of the client.
     * @return True if connected to the server, false otherwise.
     */
    public boolean isConnected() { return connected; }
    
    /**
     * Performs an internal cleanup of socket resources without triggering event listeners.
     */
    private synchronized void safeCloseInternal() {
        connected = false;
        try { if (in != null) in.close(); } catch (Throwable ignored) {}
        try { if (out != null) out.close(); } catch (Throwable ignored) {}
        try { if (socket != null) socket.close(); } catch (Throwable ignored) {}
    }
}