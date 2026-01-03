import javax.swing.*;
import java.awt.*;
import javax.imageio.ImageIO;
import java.io.File;
import java.io.IOException;

public class LobbyPanel extends JPanel {
    private final DefaultListModel<String> roomListModel;
    private final JList<String> roomList;
    private final JButton btnJoinRoom;
    private final JButton btnCreateRoom;
    private final JButton btnRefreshRooms;
    private final JButton btnDisconnect; 
    private final JButton btnExit;
    
    private String lastRoomListPayload = "";
    
    /* [FIX] Background Image support */
    private Image bgImage = null;

    public LobbyPanel(ChessClient controller) {
        setLayout(new BorderLayout());
        // setBackground(new Color(60, 60, 60)); // Removed solid bg
        
        /* [FIX] Load Background */
        try {
            bgImage = ImageIO.read(new File("backgrounds", "chessboardbg.jpg"));
        } catch (Exception e) {
            // fallback if missing
            setBackground(new Color(60, 60, 60));
        }

        JLabel title = new JLabel("LOBBY - Select a Room", SwingConstants.CENTER);
        title.setFont(new Font("SansSerif", Font.BOLD, 24));
        title.setForeground(Color.WHITE);
        title.setBorder(BorderFactory.createEmptyBorder(20, 0, 20, 0));
        add(title, BorderLayout.NORTH);

        roomListModel = new DefaultListModel<>();
        roomList = new JList<>(roomListModel);
        roomList.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
        roomList.setFont(new Font("Monospaced", Font.PLAIN, 16));
        
        /* [FIX] Transparent List and ScrollPane */
        roomList.setOpaque(false);
        roomList.setBackground(new Color(0, 0, 0, 0)); // Fully transparent base, cell renderer handles text
        roomList.setForeground(Color.WHITE);
        
        // Custom renderer to make text readable on bg
        roomList.setCellRenderer(new DefaultListCellRenderer() {
            @Override
            public Component getListCellRendererComponent(JList<?> list, Object value, int index, boolean isSelected, boolean cellHasFocus) {
                super.getListCellRendererComponent(list, value, index, isSelected, cellHasFocus);
                setOpaque(true);
                if (isSelected) {
                    setBackground(list.getSelectionBackground());
                    setForeground(list.getSelectionForeground());
                } else {
                    // Semi-transparent black for unselected items
                    setBackground(new Color(0, 0, 0, 150));
                    setForeground(Color.WHITE);
                }
                return this;
            }
        });

        JScrollPane scroll = new JScrollPane(roomList);
        scroll.setBorder(BorderFactory.createEmptyBorder(10, 50, 10, 50));
        scroll.setOpaque(false);
        scroll.getViewport().setOpaque(false);
        add(scroll, BorderLayout.CENTER);

        JPanel btnPanel = new JPanel(new FlowLayout(FlowLayout.CENTER, 20, 20));
        btnPanel.setOpaque(false);

        btnCreateRoom = new JButton("Create New Room");
        btnJoinRoom = new JButton("Join Selected Room");
        btnRefreshRooms = new JButton("Refresh List");
        btnDisconnect = new JButton("Disconnect"); 
        btnExit = new JButton("Exit");

        // Action Listeners
        btnCreateRoom.addActionListener(e -> controller.sendNetworkCommand(Protocol.CMD_NEW));
        btnRefreshRooms.addActionListener(e -> controller.sendNetworkCommand(Protocol.CMD_LIST));
        
        btnJoinRoom.addActionListener(e -> {
            String sel = roomList.getSelectedValue();
            if (sel != null) {
                String[] parts = sel.split(":");
                if (parts.length > 0) controller.sendNetworkCommand(Protocol.CMD_JOIN + " " + parts[0]);
            }
        });
        
        btnDisconnect.addActionListener(e -> controller.disconnect());
        btnExit.addActionListener(e -> controller.handleDisconnectAndExit());

        btnJoinRoom.setEnabled(false);
        roomList.addListSelectionListener(e -> btnJoinRoom.setEnabled(!roomList.isSelectionEmpty()));

        btnPanel.add(btnCreateRoom);
        btnPanel.add(btnJoinRoom);
        btnPanel.add(btnRefreshRooms);
        btnPanel.add(btnDisconnect); 
        btnPanel.add(btnExit);
        add(btnPanel, BorderLayout.SOUTH);
    }
    
    /* [FIX] Custom painting for background */
    @Override
    protected void paintComponent(Graphics g) {
        super.paintComponent(g);
        if (bgImage != null) {
            g.drawImage(bgImage, 0, 0, getWidth(), getHeight(), this);
        } else {
            g.setColor(new Color(60, 60, 60));
            g.fillRect(0, 0, getWidth(), getHeight());
        }
    }

    public void updateRoomList(String payload) {
        if (payload.equals(lastRoomListPayload)) return;
        lastRoomListPayload = payload;

        SwingUtilities.invokeLater(() -> {
            String selectedVal = roomList.getSelectedValue();
            roomListModel.clear();
            if (!payload.equals("EMPTY") && !payload.isEmpty()) {
                String[] rooms = payload.split(" ");
                for (String r : rooms) if (!r.isEmpty()) roomListModel.addElement(r);
            }
            if (selectedVal != null && roomListModel.contains(selectedVal)) {
                roomList.setSelectedValue(selectedVal, true);
            }
        });
    }
    
    public void reset() {
        lastRoomListPayload = "";
        roomListModel.clear();
    }
}