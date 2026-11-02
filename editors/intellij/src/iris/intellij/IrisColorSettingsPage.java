package iris.intellij;

import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.fileTypes.SyntaxHighlighter;
import com.intellij.openapi.options.colors.AttributesDescriptor;
import com.intellij.openapi.options.colors.ColorDescriptor;
import com.intellij.openapi.options.colors.ColorSettingsPage;

import javax.swing.Icon;
import java.util.Map;

public final class IrisColorSettingsPage implements ColorSettingsPage {
    private static final AttributesDescriptor[] DESCRIPTORS = {
        new AttributesDescriptor("Comments//Line comment", IrisSyntaxHighlighter.LINE_COMMENT),
        new AttributesDescriptor("Comments//Block comment", IrisSyntaxHighlighter.BLOCK_COMMENT),
        new AttributesDescriptor("String", IrisSyntaxHighlighter.STRING),
        new AttributesDescriptor("Number", IrisSyntaxHighlighter.NUMBER),
        new AttributesDescriptor("Keyword", IrisSyntaxHighlighter.KEYWORD),
        new AttributesDescriptor("true, false, null, self", IrisSyntaxHighlighter.CONSTANT),
        new AttributesDescriptor("Built-in type", IrisSyntaxHighlighter.PRIMITIVE),
        new AttributesDescriptor("Type name", IrisSyntaxHighlighter.TYPE_NAME),
        new AttributesDescriptor("Function call", IrisSyntaxHighlighter.FUNCTION),
        new AttributesDescriptor("Built-in function", IrisSyntaxHighlighter.BUILTIN),
        new AttributesDescriptor("Attribute", IrisSyntaxHighlighter.ATTRIBUTE),
        new AttributesDescriptor("Identifier", IrisSyntaxHighlighter.IDENTIFIER),
        new AttributesDescriptor("Braces and operators//Parentheses", IrisSyntaxHighlighter.PARENS),
        new AttributesDescriptor("Braces and operators//Braces", IrisSyntaxHighlighter.BRACES),
        new AttributesDescriptor("Braces and operators//Brackets", IrisSyntaxHighlighter.BRACKETS),
        new AttributesDescriptor("Braces and operators//Comma", IrisSyntaxHighlighter.COMMA),
        new AttributesDescriptor("Braces and operators//Dot", IrisSyntaxHighlighter.DOT),
        new AttributesDescriptor("Braces and operators//Operator", IrisSyntaxHighlighter.OPERATOR),
        new AttributesDescriptor("Bad character", IrisSyntaxHighlighter.BAD_CHARACTER),
    };

    @Override
    public Icon getIcon() {
        return IrisIcons.FILE;
    }

    @Override
    public SyntaxHighlighter getHighlighter() {
        return new IrisSyntaxHighlighter();
    }

    @Override
    public String getDemoText() {
        return """
            // a server that answers every line
            import net

            @json
            struct Point {
                i64 x
                i64 y
            }

            !void main() {
                l := try net.listen("127.0.0.1:8080")
                for {
                    c := try l.accept()
                    spawn serve(c)
                }
                sleep(250ms)
                print("{1 + 2} done\\n")
            }

            test "points add up" {
                Point p = Point{x: 1, y: 2}
                expect(p.x + p.y == 3)
            }
            """;
    }

    @Override
    public Map<String, TextAttributesKey> getAdditionalHighlightingTagToDescriptorMap() {
        return null;
    }

    @Override
    public AttributesDescriptor[] getAttributeDescriptors() {
        return DESCRIPTORS;
    }

    @Override
    public ColorDescriptor[] getColorDescriptors() {
        return ColorDescriptor.EMPTY_ARRAY;
    }

    @Override
    public String getDisplayName() {
        return "Iris";
    }
}
