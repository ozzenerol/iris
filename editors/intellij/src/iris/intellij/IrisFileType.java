package iris.intellij;

import com.intellij.openapi.fileTypes.LanguageFileType;

import javax.swing.Icon;

public final class IrisFileType extends LanguageFileType {
    public static final IrisFileType INSTANCE = new IrisFileType();

    private IrisFileType() {
        super(IrisLanguage.INSTANCE);
    }

    @Override
    public String getName() {
        return "Iris";
    }

    @Override
    public String getDescription() {
        return "Iris source";
    }

    @Override
    public String getDefaultExtension() {
        return "iris";
    }

    @Override
    public Icon getIcon() {
        return IrisIcons.FILE;
    }
}
