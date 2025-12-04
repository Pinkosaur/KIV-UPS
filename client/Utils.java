import java.awt.Point;
import java.util.Random;

/**
 * Utilities used by the GUI:
 *  - random name generator (same arrays and formatting as original client)
 *  - coordinate conversions between model (r,c) and algebraic "e2" notation
 */
public final class Utils {
    private Utils() {}

    private static final String[] NAME_ADJECTIVES = {
        "Brilliant","Interesting","Amazing","Quick","Clever","Curious","Lucky","Silent",
        "Bold","Fierce","Gentle","Merry","Young","Old","Special","Talented"
    };
    private static final String[] NAME_NOUNS = {
        "Magician","Llama","Explorer","Fox","Pioneer","Wanderer","Scholar","Knight",
        "Builder","Artist","Nimbus"
    };

    /** Return a random name like "CleverFox42" (0..99 suffix). */
    public static String generateRandomName() {
        Random rnd = new Random();
        String adj = NAME_ADJECTIVES[rnd.nextInt(NAME_ADJECTIVES.length)];
        String noun = NAME_NOUNS[rnd.nextInt(NAME_NOUNS.length)];
        int num = rnd.nextInt(100); // 0..99
        return adj + noun + num;
    }

    /**
     * Convert model coordinates (r, c) into algebraic e.g. (7,4) -> "e1"
     * Matches the original GUI coordToAlg semantics:
     *   file = 'a' + c
     *   rank = '0' + (8 - r)
     */
    public static String coordToAlg(int r, int c) {
        char file = (char)('a' + c);
        char rank = (char)('0' + (8 - r));
        return "" + file + rank;
    }

    /**
     * Convert algebraic square like "e2" into model coordinates {r,c}.
     * Returns null if input is malformed.
     */
    public static Point algToCoord(String alg) {
        if (alg == null || alg.length() < 2) return null;
        char f = alg.charAt(0);
        char rk = alg.charAt(1);
        int c = f - 'a';
        int rank = rk - '0';
        int r = 8 - rank;
        if (r < 0 || r > 7 || c < 0 || c > 7) return null;
        return new Point(r, c);
    }

    /** Safe parse helper used when input may include extra chars */
    public static Point algToCoordSafe(String alg) {
        try {
            return algToCoord(alg);
        } catch (Exception e) {
            return null;
        }
    }
}
