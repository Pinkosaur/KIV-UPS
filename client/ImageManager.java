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
 * Handles loading and caching of chess piece images.
 * Provides on-the-fly scaling of images to match the current GUI board size.
 */
public class ImageManager {
    public static final String DEFAULT_PIECES_DIR = "pieces";
    private final String piecesDir;

    // Cache raw images
    private final Map<Character, BufferedImage> pieceImagesRaw = new HashMap<>();
    // Cache scaled instances by size
    private final Map<Integer, Map<Character, ImageIcon>> iconCacheBySize = new HashMap<>();

    public ImageManager() {
        this(DEFAULT_PIECES_DIR);
    }

    public ImageManager(String piecesDir) {
        this.piecesDir = piecesDir;
    }

    /**
     * Loads raw piece images from the configured directory.
     * Expects standard filenames (e.g., "white-pawn.png").
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
     * Retrieves a scaled ImageIcon for the specified piece and cell size.
     * Uses a multi-level cache to avoid repeated scaling operations.
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

    /**
     * Clears the cache for a specific size. Useful when the window is resized.
     */
    public void clearCacheForSize(int size) {
        if (size <= 0) return;
        iconCacheBySize.remove(size);
    }

    public void clearAll() {
        pieceImagesRaw.clear();
        iconCacheBySize.clear();
    }

    public BufferedImage getRawImage(char p) {
        return pieceImagesRaw.get(p);
    }
}