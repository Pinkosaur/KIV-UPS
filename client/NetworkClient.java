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
 * Handles low-level TCP socket communication.
 * Manages separate threads for reading, writing, and heartbeats.
 * Provides a listener interface for upper layers to handle messages and events.
 */
public class NetworkClient {
    public interface NetworkListener {
        void onConnected();
        void onDisconnected();
        void onServerMessage(String line, int seq);
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
    private final Object seqLock = new Object();
    private int seqOutbound = -1; 
    
    private final DateTimeFormatter timeFmt = DateTimeFormatter.ofPattern("HH:mm:ss.SSS");

    public NetworkClient(NetworkListener listener) {
        this.listener = listener;
    }

    private void log(String prefix, String msg) {
        System.out.println("[" + LocalTime.now().format(timeFmt) + "] " + prefix + " " + msg);
    }

    /**
     * Establishes a TCP connection to the server.
     * Starts reader, writer, and heartbeat threads.
     * Logs the local port upon successful connection.
     */
    public synchronized void connect(String host, int port, String clientName, String sessionID) throws IOException {
        if (connected) return;
        closed = false;

        try {
            log("CONN", "Connecting to " + host + ":" + port + " as " + clientName);
            socket = new Socket(host, port);
            socket.setTcpNoDelay(true);
            
            // Log local port information
            log("CONN", "Local connection established on port: " + socket.getLocalPort());

            in = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
            out = new PrintWriter(new OutputStreamWriter(socket.getOutputStream(), StandardCharsets.UTF_8), true);

            connected = true;
            lastRxTime = System.currentTimeMillis(); 

            readerThread = new Thread(this::readLoop);
            readerThread.setDaemon(true);
            readerThread.start();
            
            heartbeatThread = new Thread(this::heartbeatLoop);
            heartbeatThread.setDaemon(true);
            heartbeatThread.start();
            
            writerThread = new Thread(this::writeLoop);
            writerThread.setDaemon(true);
            writerThread.start();

            if (listener != null) listener.onConnected();
            
            sendRaw("HELLO " + clientName + " " + sessionID);

        } catch (IOException ex) {
            log("ERR", "Connect failed: " + ex.getMessage());
            closeConnection();
            throw ex; 
        }
    }
    
    private void writeLoop() {
        while (connected && !closed) {
            try {
                String msg = writeQueue.take();
                if (out != null) {
                    log("[RAW OUT]", msg);
                    out.println(msg);
                    
                    if (out.checkError()) {
                        IOException ex = new IOException("Write failed (Broken Pipe)");
                        if (listener != null) listener.onNetworkError(ex);
                        closeConnection();
                        break;
                    }
                }
            } catch (InterruptedException e) {
                break;
            } catch (Exception e) {
                e.printStackTrace();
            }
        }
    }

    private void heartbeatLoop() {
        while (connected && !closed) {
            try {
                Thread.sleep(HEARTBEAT_INTERVAL);
                if (!connected) break;

                long silence = System.currentTimeMillis() - lastRxTime;
                if (silence > READ_TIMEOUT) {
                    System.err.println("Heartbeat timeout! No data for " + silence + "ms.");
                    if (listener != null) listener.onDisconnected();
                    closeConnection(); 
                    break;
                }
                sendRaw("PING");
            } catch (InterruptedException e) {
                break;
            } catch (Exception e) { }
        }
    }

    private void readLoop() {
        try {
            String rawLine;
            while ((rawLine = in.readLine()) != null) {
                log("[RAW IN ]", rawLine);

                lastRxTime = System.currentTimeMillis();
                String line = rawLine.trim();
                if (line.isEmpty()) continue;
                if (line.equals("PNG")) continue; 

                String payload = line;
                int recvSeq = -1;
                int lastSlash = line.lastIndexOf('/');
                if (lastSlash > 0 && lastSlash < line.length() - 1) {
                    try {
                        String seqStr = line.substring(lastSlash + 1);
                        recvSeq = Integer.parseInt(seqStr);
                        payload = line.substring(0, lastSlash);
                    } catch (NumberFormatException ignored) {}
                }

                try {
                    if (listener != null) listener.onServerMessage(payload, recvSeq);
                } catch (Exception ex) { ex.printStackTrace(); }
            }
            log("INFO", "Socket EOF (Server closed connection)");
            if (listener != null) listener.onDisconnected();
        } catch (IOException ex) {
            log("ERR", "Read Error: " + ex.getMessage());
            if (!closed && listener != null) listener.onNetworkError(ex);
        } finally {
            safeCloseInternal();
        }
    }
    
    public void sendRaw(String msg) {
        if (!connected || closed) return;
        writeQueue.offer(msg);
    }

    public void sendMove(String mv) {
        synchronized(seqLock) {
            sendRaw("MV" + mv); 
        }
    }
    
    public void syncSequenceFromReception(int serverSeq) {
        synchronized(seqLock) {
            this.seqOutbound = serverSeq;
        }
    }

    public synchronized void closeConnection() {
        if (closed) return;
        log("INFO", "Closing connection...");
        closed = true;
        connected = false;
        
        try { if (heartbeatThread != null) heartbeatThread.interrupt(); } catch(Exception e){}
        try { if (readerThread != null) readerThread.interrupt(); } catch(Exception e){}
        try { if (writerThread != null) writerThread.interrupt(); } catch(Exception e){}
        
        try { if (in != null) in.close(); } catch (Exception e) {}
        try { if (out != null) out.close(); } catch (Exception e) {}
        try { if (socket != null) socket.close(); } catch (Exception e) {}
        
        writeQueue.clear();
    }
    
    public boolean isConnected() { return connected; }
    
    private synchronized void safeCloseInternal() {
        connected = false;
        try { if (in != null) in.close(); } catch (Throwable ignored) {}
        try { if (out != null) out.close(); } catch (Throwable ignored) {}
        try { if (socket != null) socket.close(); } catch (Throwable ignored) {}
    }
}