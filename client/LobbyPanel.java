import javax.swing.*;
import java.awt.*;

public class LobbyPanel extends JPanel {
    private final DefaultListModel<String> roomListModel;
    private final JList<String> roomList;
    private final JButton btnJoinRoom;
    private final JButton btnCreateRoom;
    private final JButton btnRefreshRooms;
    private final JButton btnDisconnect; // [NEW]
    private final JButton btnExit;
    
    private String lastRoomListPayload = "";

    public LobbyPanel(ChessClient controller) {
        setLayout(new BorderLayout());
        setBackground(new Color(60, 60, 60));

        JLabel title = new JLabel("LOBBY - Select a Room", SwingConstants.CENTER);
        title.setFont(new Font("SansSerif", Font.BOLD, 24));
        title.setForeground(Color.WHITE);
        title.setBorder(BorderFactory.createEmptyBorder(20, 0, 20, 0));
        add(title, BorderLayout.NORTH);

        roomListModel = new DefaultListModel<>();
        roomList = new JList<>(roomListModel);
        roomList.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
        roomList.setFont(new Font("Monospaced", Font.PLAIN, 16));
        JScrollPane scroll = new JScrollPane(roomList);
        scroll.setBorder(BorderFactory.createEmptyBorder(10, 50, 10, 50));
        add(scroll, BorderLayout.CENTER);

        JPanel btnPanel = new JPanel(new FlowLayout(FlowLayout.CENTER, 20, 20));
        btnPanel.setOpaque(false);

        btnCreateRoom = new JButton("Create New Room");
        btnJoinRoom = new JButton("Join Selected Room");
        btnRefreshRooms = new JButton("Refresh List");
        btnDisconnect = new JButton("Disconnect"); // [NEW]
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
        
        /* [NEW] Disconnect Action */
        btnDisconnect.addActionListener(e -> controller.disconnect());
        
        btnExit.addActionListener(e -> controller.handleDisconnectAndExit());

        btnJoinRoom.setEnabled(false);
        roomList.addListSelectionListener(e -> btnJoinRoom.setEnabled(!roomList.isSelectionEmpty()));

        btnPanel.add(btnCreateRoom);
        btnPanel.add(btnJoinRoom);
        btnPanel.add(btnRefreshRooms);
        btnPanel.add(btnDisconnect); // [NEW]
        btnPanel.add(btnExit);
        add(btnPanel, BorderLayout.SOUTH);
    }

    public void updateRoomList(String payload) {
        // Optimization: prevent flicker if list hasn't changed
        if (payload.equals(lastRoomListPayload)) return;
        lastRoomListPayload = payload;

        SwingUtilities.invokeLater(() -> {
            String selectedVal = roomList.getSelectedValue();
            roomListModel.clear();
            if (!payload.equals("EMPTY") && !payload.isEmpty()) {
                String[] rooms = payload.split(" ");
                for (String r : rooms) if (!r.isEmpty()) roomListModel.addElement(r);
            }
            // Restore selection
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