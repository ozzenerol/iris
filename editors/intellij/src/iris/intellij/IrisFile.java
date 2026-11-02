package iris.intellij;

import com.intellij.extapi.psi.PsiFileBase;
import com.intellij.openapi.fileTypes.FileType;
import com.intellij.psi.FileViewProvider;

public final class IrisFile extends PsiFileBase {
    public IrisFile(FileViewProvider viewProvider) {
        super(viewProvider, IrisLanguage.INSTANCE);
    }

    @Override
    public FileType getFileType() {
        return IrisFileType.INSTANCE;
    }

    @Override
    public String toString() {
        return "Iris file";
    }
}
