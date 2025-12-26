import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ThreadLocalRandom;

/**
 * NetworkClient
 *
 * Handles TCP connection and the "/NNN" sequence protocol.
 * * KEY FIX: 
 * - Adopts the server's sequence number upon reception (Shared Sequence Model).
 * - Does not mistakenly send ACKs back to the server (which caused "Unknown Command" errors).
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
            // Disable Nagle's algorithm for lower latency
            socket.setTcpNoDelay(true); 
            
            in = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
            out = new PrintWriter(new OutputStreamWriter(socket.getOutputStream(), StandardCharsets.UTF_8), true);
            connected = true;

            // Initialize random seq (0..511)
            synchronized (seqLock) {
                seqOutbound = ThreadLocalRandom.current().nextInt(512);
            }

            try { listener.onConnected(); } catch (Exception ex) { ex.printStackTrace(); }

            if (clientName != null) {
                int used = sendRaw("HELLO " + clientName);
                System.out.println("Internal: sent HELLO with seq=" + used);
            }

            readerThread = new Thread(this::readLoop, "NetworkClient-Reader");
            readerThread.setDaemon(true);
            readerThread.start();
        } catch (IOException ex) {
            safeCloseInternal();
            connected = false;
            try { listener.onNetworkError(ex); } catch (Exception ignore) {}
        }
    }

    public boolean isConnected() { return connected && !closed && socket != null && !socket.isClosed(); }

    /**
     * Send a line to the server appending "/NNN".
     * Updates local seqOutbound to (seq + 1) % 512.
     */
    public synchronized int sendRaw(String line) {
        if (out == null || closed) return -1;
        int usedSeq;
        synchronized (seqLock) {
            if (seqOutbound < 0) seqOutbound = 0;
            usedSeq = seqOutbound;
            seqOutbound = (seqOutbound + 1) % 512;
        }
        String s = String.format("%03d", usedSeq);
        String lineWithSeq = line + "/" + s;
        try {
            out.println(lineWithSeq);
            out.flush();
            System.out.println(">> " + lineWithSeq);
            return usedSeq;
        } catch (Exception ex) {
            try { listener.onNetworkError(ex); } catch (Exception ignore) {}
            safeCloseInternal();
            return -1;
        }
    }
    
    public int sendMove(String move) {
        if (move == null) return -1;
        return sendRaw("MV" + move);
    }

    public synchronized void closeConnection() {
        if (closed) return;
        closed = true;
        safeCloseInternal();
    }
    
    /**
     * Called when we receive a valid sequence number from the server.
     * We must update our next outbound sequence to match (seq + 1), 
     * ensuring we stay in sync with the server's state.
     */
    public void syncSequenceFromReception(int recvSeq) {
        synchronized (seqLock) {
            // Next message we send should be recvSeq + 1
            seqOutbound = (recvSeq + 1) % 512;
        }
    }

    private void readLoop() {
        try {
            String rawLine;
            while (!closed && (rawLine = in.readLine()) != null) {
                // Parse trailing /NNN
                String payload = rawLine;
                int recvSeq = -1;
                int lastSlash = rawLine.lastIndexOf('/');
                
                // robust check: ensures last part is exactly 3 digits
                if (lastSlash >= 0 && rawLine.length() - lastSlash - 1 == 3) {
                    String seqStr = rawLine.substring(lastSlash + 1);
                    boolean allDigits = true;
                    for (int i = 0; i < seqStr.length(); ++i) {
                        if (!Character.isDigit(seqStr.charAt(i))) { 
                            allDigits = false; 
                            break; 
                        }
                    }
                    if (allDigits) {
                        try {
                            recvSeq = Integer.parseInt(seqStr);
                            payload = rawLine.substring(0, lastSlash);
                        } catch (NumberFormatException ignored) {}
                    }
                }

                try {
                    listener.onServerMessage(payload, recvSeq);
                } catch (Exception ex) {
                    ex.printStackTrace();
                }
            }
            try { listener.onDisconnected(); } catch (Exception ex) {}
        } catch (IOException ex) {
            if (!closed) {
                try { listener.onNetworkError(ex); } catch (Exception ignore) {}
            }
        } finally {
            safeCloseInternal();
        }
    }

    private synchronized void safeCloseInternal() {
        connected = false;
        try { if (in != null) in.close(); } catch (Throwable ignored) {}
        try { if (out != null) out.close(); } catch (Throwable ignored) {}
        try { if (socket != null) socket.close(); } catch (Throwable ignored) {}
        try { if (readerThread != null) readerThread.interrupt(); } catch (Throwable ignored) {}
    }
}