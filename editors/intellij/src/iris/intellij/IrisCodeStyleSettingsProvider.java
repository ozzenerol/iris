package iris.intellij;

import com.intellij.application.options.IndentOptionsEditor;
import com.intellij.application.options.SmartIndentOptionsEditor;
import com.intellij.lang.Language;
import com.intellij.psi.codeStyle.CommonCodeStyleSettings;
import com.intellij.psi.codeStyle.LanguageCodeStyleSettingsProvider;

/** Iris indents with 4 spaces, the layout iris fmt writes. */
public final class IrisCodeStyleSettingsProvider extends LanguageCodeStyleSettingsProvider {
    @Override
    public Language getLanguage() {
        return IrisLanguage.INSTANCE;
    }

    @Override
    protected void customizeDefaults(CommonCodeStyleSettings settings, CommonCodeStyleSettings.IndentOptions indent) {
        indent.INDENT_SIZE = 4;
        indent.CONTINUATION_INDENT_SIZE = 4;
        indent.TAB_SIZE = 4;
        indent.USE_TAB_CHARACTER = false;
    }

    @Override
    public IndentOptionsEditor getIndentOptionsEditor() {
        return new SmartIndentOptionsEditor();
    }

    @Override
    public String getCodeSample(SettingsType settingsType) {
        return """
            void main() {
                for i in 0..3 {
                    print("{i}")
                }
            }
            """;
    }
}
