/**
 * Protocol
 *
 * Defines string constants for the text-based application protocol.
 * Includes commands sent by the client, response headers sent by the server,
 * and the acknowledgment codes used for transmission verification.
 */
public class Protocol {
    // --- Client Commands ---
    public static final String CMD_HELLO = "HELLO";
    public static final String CMD_MV = "MV";
    public static final String CMD_RES = "RES";
    public static final String CMD_DRW_OFF = "DRW_OFF";
    public static final String CMD_DRW_ACC = "DRW_ACC";
    public static final String CMD_DRW_DEC = "DRW_DEC";
    public static final String CMD_EXT = "EXT";
    public static final String CMD_LIST = "LIST";
    public static final String CMD_JOIN = "JOIN";
    public static final String CMD_NEW = "NEW";
    public static final String CMD_PING = "PING";

    // --- Server Responses ---
    public static final String RESP_WELCOME = "WELCOME";
    public static final String RESP_RESUME = "RESUME";
    public static final String RESP_OPP_RESUME = "OPP_RESUME";
    public static final String RESP_HISTORY = "HISTORY";
    public static final String RESP_TIME = "TIME";
    public static final String RESP_WAIT_CONN = "WAIT_CONN";
    public static final String RESP_LOBBY = "LOBBY";
    public static final String RESP_ROOMLIST = "ROOMLIST";
    public static final String RESP_WAITING = "WAITING";
    public static final String RESP_START = "START";
    public static final String RESP_OK_MV = "OK_MV";
    public static final String RESP_OPP_MV = "OPP_MV";
    public static final String RESP_CHK = "CHK";
    public static final String RESP_ERR = "ERR";
    public static final String RESP_FULL = "FULL";
    public static final String RESP_PING = "PNG";

    
    // --- End Game Conditions ---
    public static final String RESP_WIN_CHKM = "WIN_CHKM";
    public static final String RESP_CHKM = "CHKM"; // Defeat
    public static final String RESP_SM = "SM";
    public static final String RESP_OPP_RES = "OPP_RES";
    public static final String RESP_OPP_EXT = "OPP_EXT";
    public static final String RESP_RES = "RES"; // You resigned
    public static final String RESP_OPP_TOUT = "OPP_TOUT";
    public static final String RESP_TOUT = "TOUT";
    public static final String RESP_DRW_ACD = "DRW_ACD";
    public static final String RESP_DRW_OFF = "DRW_OFF";
    public static final String RESP_DRW_DCD = "DRW_DCD";

    // --- Acknowledgement Codes ---
    public static final String ACK_MATCHMAKING_TOUT = "01";
    public static final String ACK_WAIT = "02";
    public static final String ACK_START = "03";
    public static final String ACK_ERR = "04";
    public static final String ACK_ACCEPT_MOVE = "05";
    public static final String ACK_OPP_MOVE = "06";
    public static final String ACK_CHECK = "07";
    public static final String ACK_LOST_BY_CHKM = "08";
    public static final String ACK_WIN_BY_CHKM = "09";
    public static final String ACK_DRW_OFF_SC = "10";
    public static final String ACK_DRW_DEC = "11";
    public static final String ACK_DRW_ACC = "12";
    public static final String ACK_RESIGN_SC = "13";
    public static final String ACK_OPP_RESIGN = "14";
    public static final String ACK_TOUT = "15";
    public static final String ACK_OPP_TOUT = "16";
    public static final String ACK_OPP_QUIT = "17";
    public static final String ACK_HELLO = "18";
    public static final String ACK_MOVE_CMD = "19";
    public static final String ACK_DRW_OFF_CS = "20";
    public static final String ACK_DRW_DEC_CS = "21";
    public static final String ACK_DRW_ACC_CS = "22";
    public static final String ACK_RESIGN_CS = "23";
    public static final String ACK_STALEMATE = "25";
    public static final String ACK_RESUME = "26";
    public static final String ACK_LOBBY = "27";
    public static final String ACK_NEW_ROOM = "28";
    public static final String ACK_JOIN = "29";
    public static final String ACK_LIST = "30";
    public static final String ACK_EXIT = "31";
    public static final String ACK_GENERIC = "99";

    /**
     * Determines the appropriate acknowledgement code for a received message.
     * @param msg The message received from the server.
     * @return The 2-digit ACK code string.
     */
    public static String getAckFor(String msg) {
        if (msg == null || msg.isEmpty()) return ACK_GENERIC;

        // Match server commands to specific ACKs
        if (msg.startsWith(RESP_WAITING))    return ACK_WAIT;
        if (msg.startsWith(RESP_START))      return ACK_START;
        if (msg.startsWith(RESP_OK_MV))      return ACK_ACCEPT_MOVE;
        if (msg.startsWith(RESP_OPP_MV))     return ACK_OPP_MOVE;
        if (msg.equals(RESP_CHK))               return ACK_CHECK;
        if (msg.startsWith(RESP_WIN_CHKM))   return ACK_WIN_BY_CHKM;
        if (msg.startsWith(RESP_CHKM))       return ACK_LOST_BY_CHKM;
        if (msg.startsWith(RESP_SM))         return ACK_STALEMATE;
        if (msg.startsWith(RESP_DRW_OFF))    return ACK_DRW_OFF_SC;
        if (msg.startsWith(RESP_DRW_ACD))    return ACK_DRW_ACC;
        if (msg.startsWith(RESP_DRW_DCD))    return ACK_DRW_DEC;
        if (msg.startsWith(RESP_RES))        return ACK_RESIGN_SC;
        if (msg.startsWith(RESP_OPP_RES))    return ACK_OPP_RESIGN;
        if (msg.startsWith(RESP_TOUT))       return ACK_TOUT;
        if (msg.startsWith(RESP_OPP_TOUT))   return ACK_OPP_TOUT;
        if (msg.startsWith(RESP_OPP_EXT))    return ACK_OPP_QUIT;
        if (msg.startsWith(RESP_RESUME))     return ACK_RESUME;
        if (msg.startsWith(RESP_LOBBY))      return ACK_LOBBY;
        if (msg.startsWith(RESP_ERR))        return ACK_ERR;
        
        // For commands without specific ACKs (e.g. ROOMLIST, TIME, HISTORY), return Generic
        return ACK_GENERIC;
    }
}