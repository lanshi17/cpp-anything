import demo.TextTool;

/**
 * Calls the four fixture functions through the generated shadow binding. The
 * shadow class forwards to the original and compares the C++ port in the
 * background; this program only checks the original's contract is preserved.
 */
public final class Smoke {
    private static int failures = 0;

    public static void main(String[] args) {
        check("wordCount(\"Hello World\")", CppanShadow.wordCount("Hello World"), 2);
        check("wordCount(\"  Mixed CASE  \")", CppanShadow.wordCount("  Mixed CASE  "), 2);
        check("slug(\"Hello World\")", CppanShadow.slug("Hello World"), "hello-world");
        check("slug(\"  Mixed CASE  \")", CppanShadow.slug("  Mixed CASE  "), "mixed-case");
        check("compareLengths(\"a\", \"bb\")", CppanShadow.compareLengths("a", "bb"), -1);
        check("compareLengths(\"bb\", \"a\")", CppanShadow.compareLengths("bb", "a"), 1);
        check("compareInts(2, 1)", CppanShadow.compareInts(2, 1), 1);
        check("compareInts(3, 3)", CppanShadow.compareInts(3, 3), 0);
        try {
            CppanShadow.wordCount("");
            System.out.println("FAIL wordCount(\"\") did not throw");
            failures++;
        } catch (IllegalArgumentException expected) {
            System.out.println("ok   wordCount(\"\") threw IllegalArgumentException");
        }
        // The original class is still reachable and agrees; the shadow never lies.
        check("original wordCount(\"a b c\")", TextTool.wordCount("a b c"), 3);
        if (failures > 0) {
            System.out.println(failures + " smoke check(s) failed");
            System.exit(1);
        }
        System.out.println("smoke: all checks passed through CppanShadow");
    }

    private static void check(String label, Object got, Object want) {
        if (got.equals(want)) {
            System.out.println("ok   " + label + " = " + got);
        } else {
            System.out.println("FAIL " + label + ": got " + got + ", want " + want);
            failures++;
        }
    }
}
