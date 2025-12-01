import javax.imageio.ImageIO;
import javax.swing.ImageIcon;
import java.awt.Image;
import java.awt.image.BufferedImage;
import java.io.File;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;

/**
 * ImageManager
 *
 * Loads raw piece images from a configurable directory and provides cached scaled
 * ImageIcons keyed by requested cell size (so GUI can call getScaledIconForPiece(p,size)).
 *
 * By default it uses "client/pieces" which matches the original client resource layout.
 */
public class ImageManager {
    public static final String DEFAULT_PIECES_DIR = "pieces";
    private final String piecesDir;

    // raw piece images (original sized)
    private final Map<Character, BufferedImage> pieceImagesRaw = new HashMap<>();
    // cache by cell size -> (pieceChar -> ImageIcon)
    private final Map<Integer, Map<Character, ImageIcon>> iconCacheBySize = new HashMap<>();

    public ImageManager() {
        this(DEFAULT_PIECES_DIR);
    }

    public ImageManager(String piecesDir) {
        this.piecesDir = piecesDir;
    }

    /**
     * Load piece images from the pieces directory.
     * File names are the same as in the original client:
     *   white-pawn.png, white-knight.png, white-bishop.png, white-rook.png, white-queen.png, white-king.png
     *   black-pawn.png, black-knight.png, black-bishop.png, black-rook.png, black-queen.png, black-king.png
     *
     * Missing files are reported to stderr but do not throw exceptions.
     */
    public void loadIcons() {
        Map<Character, String> map = new HashMap<>();
        map.put('P', "white-pawn.png");
        map.put('N', "white-knight.png");
        map.put('B', "white-bishop.png");
        map.put('R', "white-rook.png");
        map.put('Q', "white-queen.png");
        map.put('K', "white-king.png");
        map.put('p', "black-pawn.png");
        map.put('n', "black-knight.png");
        map.put('b', "black-bishop.png");
        map.put('r', "black-rook.png");
        map.put('q', "black-queen.png");
        map.put('k', "black-king.png");

        pieceImagesRaw.clear();
        iconCacheBySize.clear();

        for (Map.Entry<Character, String> e : map.entrySet()) {
            File f = new File(piecesDir, e.getValue());
            if (f.exists()) {
                try {
                    BufferedImage img = ImageIO.read(f);
                    if (img != null) pieceImagesRaw.put(e.getKey(), img);
                    else System.err.println("ImageIO.read returned null for " + f.getPath());
                } catch (IOException ex) {
                    System.err.println("Failed to load " + f + " : " + ex.getMessage());
                }
            } else {
                System.err.println("Piece image not found: " + f.getPath());
            }
        }
    }

    /**
     * Return a cached ImageIcon for piece 'p' scaled to cellSize x cellSize.
     * If src image is missing or p == '.' returns null.
     */
    public ImageIcon getScaledIconForPiece(char p, int cellSize) {
        if (p == '.' || cellSize <= 0) return null;
        Map<Character, ImageIcon> cache = iconCacheBySize.get(cellSize);
        if (cache != null && cache.containsKey(p)) return cache.get(p);

        BufferedImage src = pieceImagesRaw.get(p);
        if (src == null) return null;

        Image scaled = src.getScaledInstance(cellSize, cellSize, Image.SCALE_SMOOTH);
        ImageIcon ico = new ImageIcon(scaled);

        if (cache == null) {
            cache = new HashMap<>();
            iconCacheBySize.put(cellSize, cache);
        }
        cache.put(p, ico);
        return ico;
    }

    /** Remove cached icons for a given cell size (used by GUI on resize). */
    public void clearCacheForSize(int size) {
        if (size <= 0) return;
        iconCacheBySize.remove(size);
    }

    /** Completely clear loaded raw images and icon cache. Useful for reloading resources. */
    public void clearAll() {
        pieceImagesRaw.clear();
        iconCacheBySize.clear();
    }

    /** Accessor for raw image (may return null if not loaded) */
    public BufferedImage getRawImage(char p) {
        return pieceImagesRaw.get(p);
    }
}
