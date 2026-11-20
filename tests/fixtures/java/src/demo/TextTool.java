package demo;

/**
 * Text helpers exercised by the cpp-anything Java fixture: the supported shapes
 * are String to int, String to String, (String, String) to int and int to int,
 * and wordCount raises on empty input so the recording contains an error case.
 */
public final class TextTool {
    private TextTool() {
        // Static helpers only: the analyzer reports this constructor as skipped.
    }

    /**
     * Counts whitespace-separated words (ASCII whitespace, as Java's \s).
     *
     * @throws IllegalArgumentException when the text is empty
     */
    public static int wordCount(String text) {
        if (text == null || text.isEmpty()) {
            throw new IllegalArgumentException("text is empty");
        }
        int count = 0;
        boolean inWord = false;
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            boolean space = c == ' ' || c == '\t' || c == '\n' || c == '\u000B' || c == '\f' || c == '\r';
            if (space) {
                inWord = false;
            } else if (!inWord) {
                count++;
                inWord = true;
            }
        }
        return count;
    }

    /** Lowercases ASCII letters and turns every other run into a single dash. */
    public static String slug(String text) {
        if (text == null) {
            throw new IllegalArgumentException("text is null");
        }
        StringBuilder out = new StringBuilder();
        boolean pendingDash = false;
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            char lower = (c >= 'A' && c <= 'Z') ? (char) (c + ('a' - 'A')) : c;
            if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
                if (pendingDash && out.length() > 0) {
                    out.append('-');
                }
                pendingDash = false;
                out.append(lower);
            } else {
                pendingDash = true;
            }
        }
        return out.toString();
    }

    /** Compares two strings by UTF-16 length: -1, 0 or 1. */
    public static int compareLengths(String left, String right) {
        return Integer.compare(left.length(), right.length());
    }

    /** Compares two ints: -1, 0 or 1. */
    public static int compareInts(int left, int right) {
        return Integer.compare(left, right);
    }

    /** An instance method: the analyzer reports it as skipped. */
    public String describe() {
        return "text tool";
    }
}
