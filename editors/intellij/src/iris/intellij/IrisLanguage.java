package iris.intellij;

import com.intellij.lang.Language;

public final class IrisLanguage extends Language {
    public static final IrisLanguage INSTANCE = new IrisLanguage();

    private IrisLanguage() {
        super("Iris");
    }
}
