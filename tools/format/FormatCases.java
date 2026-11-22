// Writes tests/format_java.ww: format's fractions (f, e, g) laid out as Java 21's String.format lays
// them out (flags, width, grouping, %g keeping its zeros), but rounded as C rounds them: the
// number's exact binary value, halves to even (D1 of docs/snippy-ww-checklist.html). Each value
// is rounded so first, as an exact BigDecimal, and Java only lays it out.
// Run: java tools/format/FormatCases.java > tests/format_java.ww
import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Random;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class FormatCases {
    static final Pattern ITEM = Pattern.compile("%[-#+ 0,(]*\\d*(?:\\.(\\d+))?([efgEG])");

    // `pattern` of `v`, rounded from v's exact value, halves to even.
    static String format(String pattern, double v) {
        Matcher m = ITEM.matcher(pattern);
        if (!m.find()) throw new IllegalArgumentException(pattern);
        int precision = m.group(1) == null ? 6 : Integer.parseInt(m.group(1));
        char kind = Character.toLowerCase(m.group(2).charAt(0));
        BigDecimal exact = new BigDecimal(v);
        BigDecimal rounded = kind == 'f' ? exact.setScale(precision, RoundingMode.HALF_EVEN)
            : exact.round(new MathContext(kind == 'e' ? precision + 1 : Math.max(precision, 1), RoundingMode.HALF_EVEN));
        // A BigDecimal has no -0: a negative that rounds to zero is written as -0.0 is.
        if (rounded.signum() == 0 && (v < 0 || 1 / v < 0)) return String.format(Locale.US, pattern, -0.0);
        return String.format(Locale.US, pattern, rounded);
    }

    public static void main(String[] args) {
        List<Double> values = new ArrayList<>(List.of(
            0.0, -0.0, 1.0, -1.0, 0.5, 0.25, 0.35, 0.45, 1.5, 2.5, 0.125, 0.0625, 1.00005, 12345.0, 99999.5, 9.9995,
            0.99995, 999.95, 0.1, 0.2, 0.3, 1e-4, 9.9995e-5, 1e-5, 2.5e-5, 123456789.0, 1e15, 1e16, 1e20, 1e100, 1.7976931348623157e308,
            4.9e-324, 2.2250738585072014e-308, 3.141592653589793, 2.718281828459045, 1234.5678, 0.0005, 0.00049999, 5e-7, 1e22,
            11.99, 12.0, 0.015, 0.025, 1.005, 2.675, 100.0, 1000.0, 0.001, 0.009995, 9.5, 99.5, 999.5, 0.95, 0.095,
            -0.04, -2.5, -0.5, 3.5, 0.375, 1234567.125));
        Random random = new Random(1234);
        for (int i = 0; i < 40; i++) values.add(Math.pow(10, random.nextInt(24) - 12) * random.nextDouble() * (random.nextBoolean() ? 1 : -1));
        for (int i = 0; i < 20; i++) values.add((double) Math.round(random.nextDouble() * 100000) / 1000.0);
        String[] patterns = {"%f", "%.0f", "%.1f", "%.2f", "%.3f", "%.10f", "%e", "%.0e", "%.2e", "%.4E", "%g", "%.0g", "%.1g", "%.4g", "%.6G",
                             "%,.2f", "%010.3f", "%+.1e", "%12.4g", "%-12.2f|", "% .3f", "%,g"};
        System.out.println("// WinterWolf's format writes fractions (f, e, g) laid out as Java's String.format lays them out,");
        System.out.println("// %g keeping its zeros, rounded from the exact value with halves to even, as C rounds them.");
        System.out.println("// Made by tools/format/FormatCases.java on Java 21; do not edit by hand.");
        System.out.println();
        for (double v : values) {
            String literal = Double.toString(v).replace("E", "e");
            StringBuilder line = new StringBuilder("print(");
            StringBuilder expect = new StringBuilder();
            for (int i = 0; i < patterns.length; i++) {
                if (i > 0) { line.append(" + \" \" + "); expect.append(' '); }
                line.append('"').append(patterns[i]).append("\".format(").append(literal).append(')');
                expect.append(format(patterns[i], v));
            }
            line.append(")   // expect: ").append(expect);
            System.out.println(line);
        }
        System.out.println();
        System.out.println("// Not numbers: as Java writes them.");
        System.out.println("print(\"%.2f\".format(0.0 / 0.0), \"%e\".format(1.0 / 0.0), \"%g\".format(-1.0 / 0.0))   // expect: "
            + String.format(Locale.US, "%.2f %e %g", Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY));
    }
}
