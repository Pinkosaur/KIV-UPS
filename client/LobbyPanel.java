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
    
    private final JPanel centerContainer; 
    private final CardLayout centerLayout;
    
    private String lastRoomListPayload = "";
    private Image bgImage = null;

    public LobbyPanel(ChessClient controller) {
        setLayout(new BorderLayout());
        
        try {
            bgImage = ImageIO.read(new File("backgrounds", "chessboardbg.jpg"));
        } catch (Exception e) {
            setBackground(new Color(60, 60, 60));
        }

        JLabel title = new JLabel("LOBBY - Select a Room", SwingConstants.CENTER);
        title.setFont(new Font("SansSerif", Font.BOLD, 24));
        title.setForeground(Color.WHITE);
        title.setBorder(BorderFactory.createEmptyBorder(20, 0, 20, 0));
        add(title, BorderLayout.NORTH);

        // --- Center Area ---
        centerLayout = new CardLayout();
        centerContainer = new JPanel(centerLayout);
        centerContainer.setOpaque(false);
        
        DarkPanel listBackground = new DarkPanel();
        listBackground.setLayout(new BorderLayout());
        
        roomListModel = new DefaultListModel<>();
        roomList = new JList<>(roomListModel);
        roomList.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
        roomList.setFont(new Font("Monospaced", Font.PLAIN, 16));
        roomList.setOpaque(false);
        roomList.setBackground(new Color(0,0,0,0));
        roomList.setForeground(Color.WHITE);
        
        roomList.setCellRenderer(new DefaultListCellRenderer() {
            @Override
            public Component getListCellRendererComponent(JList<?> list, Object value, int index, boolean isSelected, boolean cellHasFocus) {
                super.getListCellRendererComponent(list, value, index, isSelected, cellHasFocus);
                setOpaque(isSelected);
                if (isSelected) {
                    setBackground(new Color(0, 100, 200));
                    setForeground(Color.WHITE);
                } else {
                    setForeground(Color.WHITE);
                }
                return this;
            }
        });

        JScrollPane scroll = new JScrollPane(roomList);
        scroll.setBorder(BorderFactory.createEmptyBorder());
        scroll.setOpaque(false);
        scroll.getViewport().setOpaque(false);
        
        listBackground.add(scroll, BorderLayout.CENTER);
        
        DarkPanel emptyBackground = new DarkPanel();
        emptyBackground.setLayout(new GridBagLayout());
        JLabel emptyLabel = new JLabel("No room available at the moment.");
        emptyLabel.setFont(new Font("SansSerif", Font.ITALIC, 18));
        emptyLabel.setForeground(new Color(220, 220, 220));
        emptyBackground.add(emptyLabel);

        centerContainer.add(listBackground, "LIST");
        centerContainer.add(emptyBackground, "EMPTY");
        
        JPanel centerWrapper = new JPanel(new BorderLayout());
        centerWrapper.setOpaque(false);
        centerWrapper.setBorder(BorderFactory.createEmptyBorder(10, 50, 10, 50));
        centerWrapper.add(centerContainer, BorderLayout.CENTER);
        add(centerWrapper, BorderLayout.CENTER);

        // --- Controls ---
        JPanel btnPanel = new JPanel(new FlowLayout(FlowLayout.CENTER, 20, 20));
        btnPanel.setOpaque(false);

        btnCreateRoom = new JButton("Create New Room");
        btnJoinRoom = new JButton("Join Selected Room");
        btnRefreshRooms = new JButton("Refresh List");
        btnDisconnect = new JButton("Disconnect"); 
        btnExit = new JButton("Exit");

        // [FIX] Removed optimistic UI call. Waits for server.
        btnCreateRoom.addActionListener(e -> {
            setButtonsEnabled(false);
            controller.sendNetworkCommand(Protocol.CMD_NEW);
        });
        
        btnRefreshRooms.addActionListener(e -> {
            controller.sendNetworkCommand(Protocol.CMD_LIST);
        });
        
        btnJoinRoom.addActionListener(e -> {
            String sel = roomList.getSelectedValue();
            if (sel != null) {
                String[] parts = sel.split(":");
                if (parts.length > 0) {
                    setButtonsEnabled(false);
                    controller.sendNetworkCommand(Protocol.CMD_JOIN + " " + parts[0]);
                }
            }
        });
        
        btnDisconnect.addActionListener(e -> controller.disconnect());
        btnExit.addActionListener(e -> controller.handleDisconnectAndExit());

        btnJoinRoom.setEnabled(false);
        roomList.addListSelectionListener(e -> {
            if (btnCreateRoom.isEnabled()) {
                btnJoinRoom.setEnabled(!roomList.isSelectionEmpty());
            }
        });

        btnPanel.add(btnCreateRoom);
        btnPanel.add(btnJoinRoom);
        btnPanel.add(btnRefreshRooms);
        btnPanel.add(btnDisconnect); 
        btnPanel.add(btnExit);
        add(btnPanel, BorderLayout.SOUTH);
    }
    
    public void setButtonsEnabled(boolean enabled) {
        btnCreateRoom.setEnabled(enabled);
        btnJoinRoom.setEnabled(enabled && !roomList.isSelectionEmpty());
        btnRefreshRooms.setEnabled(enabled);
    }
    
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
        SwingUtilities.invokeLater(() -> setButtonsEnabled(true));

        if (payload.equals(lastRoomListPayload)) return;
        lastRoomListPayload = payload;

        SwingUtilities.invokeLater(() -> {
            String selectedVal = roomList.getSelectedValue();
            roomListModel.clear();
            
            boolean isEmpty = payload.equals("EMPTY") || payload.isEmpty();
            
            if (!isEmpty) {
                String[] rooms = payload.split(" ");
                boolean foundAny = false;
                for (String r : rooms) {
                    if (!r.isEmpty()) {
                        roomListModel.addElement(r);
                        foundAny = true;
                    }
                }
                isEmpty = !foundAny;
            }
            
            if (isEmpty) {
                centerLayout.show(centerContainer, "EMPTY");
            } else {
                centerLayout.show(centerContainer, "LIST");
                if (selectedVal != null && roomListModel.contains(selectedVal)) {
                    roomList.setSelectedValue(selectedVal, true);
                }
            }
        });
    }
    
    public void reset() {
        lastRoomListPayload = "";
        roomListModel.clear();
        centerLayout.show(centerContainer, "EMPTY");
        setButtonsEnabled(true);
    }
    
    private static class DarkPanel extends JPanel {
        private static final Color SEMI_TRANSPARENT_BG = new Color(0, 0, 0, 150);
        
        public DarkPanel() {
            setOpaque(false);
        }
        
        @Override
        protected void paintComponent(Graphics g) {
            g.setColor(SEMI_TRANSPARENT_BG);
            g.fillRect(0, 0, getWidth(), getHeight());
            super.paintComponent(g);
        }
    }
}