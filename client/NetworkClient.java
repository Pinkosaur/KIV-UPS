import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

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
    
    // Heartbeat components
    private Thread heartbeatThread;
    private volatile long lastRxTime = 0;
    private static final long HEARTBEAT_INTERVAL = 2000; // Send PING every 2s
    private static final long READ_TIMEOUT = 10000;      // Die if no data for 10s

    private volatile boolean closed = false;
    private volatile boolean connected = false;
    private final Object seqLock = new Object();
    private int seqOutbound = -1; 

    public NetworkClient(NetworkListener listener) {
        this.listener = listener;
    }

    public synchronized void connect(String host, int port, String clientName) {
        if (connected) return;
        closed = false;

        try {
            socket = new Socket(host, port);
            socket.setTcpNoDelay(true);
            in = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
            out = new PrintWriter(new OutputStreamWriter(socket.getOutputStream(), StandardCharsets.UTF_8), true);

            connected = true;
            lastRxTime = System.currentTimeMillis(); // Init timer

            // 1. Reader Thread
            readerThread = new Thread(this::readLoop);
            readerThread.setDaemon(true);
            readerThread.start();
            
            // 2. Heartbeat Thread
            heartbeatThread = new Thread(this::heartbeatLoop);
            heartbeatThread.setDaemon(true);
            heartbeatThread.start();

            if (listener != null) listener.onConnected();
            
            // Handshake
            sendRaw("HELLO " + clientName);

        } catch (IOException ex) {
            closeConnection();
            if (listener != null) listener.onNetworkError(ex);
        }
    }
    
    // Sends PINGs and checks for Timeouts
    private void heartbeatLoop() {
        while (connected && !closed) {
            try {
                Thread.sleep(HEARTBEAT_INTERVAL);
                if (!connected) break;

                // A. Check for timeout
                long silence = System.currentTimeMillis() - lastRxTime;
                if (silence > READ_TIMEOUT) {
                    System.err.println("Heartbeat timeout! No data for " + silence + "ms.");
                    closeConnection(); // This triggers onDisconnected -> Reconnect UI
                    break;
                }

                // B. Send PING
                sendRaw("PING");

            } catch (InterruptedException e) {
                break;
            } catch (Exception e) {
                // If writing fails, reader loop or error handler will catch it
            }
        }
    }

    private void readLoop() {
        try {
            String rawLine;
            while ((rawLine = in.readLine()) != null) {
                // Update heartbeat timestamp on ANY received data
                lastRxTime = System.currentTimeMillis();

                String line = rawLine.trim();
                if (line.isEmpty()) continue;
                
                // Filter out the Heartbeat ACK so UI doesn't see it
                if (line.equals("PNG")) {
                    continue; 
                }

                // Parse Protocol: MSG/SEQ
                String payload = line;
                int recvSeq = -1;
                int lastSlash = line.lastIndexOf('/');
                if (lastSlash > 0 && lastSlash < line.length() - 1) {
                    String seqStr = line.substring(lastSlash + 1);
                    boolean allDigits = true;
                    for (int i = 0; i < seqStr.length(); ++i) {
                        if (!Character.isDigit(seqStr.charAt(i))) { allDigits = false; break; }
                    }
                    if (allDigits) {
                        try {
                            recvSeq = Integer.parseInt(seqStr);
                            payload = line.substring(0, lastSlash);
                        } catch (NumberFormatException ignored) {}
                    }
                }

                try {
                    if (listener != null) listener.onServerMessage(payload, recvSeq);
                } catch (Exception ex) { ex.printStackTrace(); }
            }
            // EOF
            if (listener != null) listener.onDisconnected();
        } catch (IOException ex) {
            if (!closed && listener != null) listener.onNetworkError(ex);
        } finally {
            closeConnection();
        }
    }
    
    public void sendRaw(String msg) {
        if (!connected || out == null) return;
        try {
            out.println(msg);
            if (out.checkError()) throw new IOException("Write failed");
        } catch (Exception e) {
            closeConnection();
        }
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
        closed = true;
        connected = false;
        try { if (heartbeatThread != null) heartbeatThread.interrupt(); } catch(Exception e){}
        try { if (in != null) in.close(); } catch (Exception e) {}
        try { if (out != null) out.close(); } catch (Exception e) {}
        try { if (socket != null) socket.close(); } catch (Exception e) {}
    }
    
    public boolean isConnected() { return connected; }
}