public class Protocol {
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
    public static final String CMD_PNG = "PNG";

    // Server responses
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
    public static final String RESP_ERR = "ERR";
    public static final String RESP_FULL = "FULL";
    
    // End game conditions
    public static final String RESP_WIN_CHKM = "WIN_CHKM";
    public static final String RESP_CHKM = "CHKM"; // Defeat
    public static final String RESP_SM = "SM";
    public static final String RESP_OPP_RES = "OPP_RES";
    public static final String RESP_OPP_EXT = "OPP_EXT";
    public static final String RESP_RES = "RES"; // You resigned
    public static final String RESP_OPP_TOUT = "OPP_TOUT";
    public static final String RESP_TOUT = "TOUT";
    public static final String RESP_DRW_ACD = "DRW_ACD"; // Draw agreed
    public static final String RESP_DRW_OFF = "DRW_OFF";
    public static final String RESP_DRW_DCD = "DRW_DCD";
}