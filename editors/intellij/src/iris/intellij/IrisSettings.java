package iris.intellij;

import com.intellij.execution.configurations.PathEnvironmentVariableUtil;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.components.PersistentStateComponent;
import com.intellij.openapi.components.State;
import com.intellij.openapi.components.Storage;

import java.io.File;

/** Where the iris program is. Empty means the one on PATH, then /usr/local/bin/iris. */
@State(name = "IrisSettings", storages = @Storage("iris.xml"))
public final class IrisSettings implements PersistentStateComponent<IrisSettings.Data> {
    public static final class Data {
        public String irisPath = "";
    }

    private Data data = new Data();

    public static IrisSettings get() {
        return ApplicationManager.getApplication().getService(IrisSettings.class);
    }

    @Override
    public Data getState() {
        return data;
    }

    @Override
    public void loadState(Data state) {
        data = state;
    }

    public String irisPath() {
        return data.irisPath == null ? "" : data.irisPath.trim();
    }

    public void setIrisPath(String path) {
        data.irisPath = path == null ? "" : path.trim();
    }

    /** The iris program to start. */
    public String iris() {
        String p = irisPath();
        if (!p.isEmpty()) {
            return p;
        }
        File found = PathEnvironmentVariableUtil.findInPath("iris");
        if (found != null) {
            return found.getAbsolutePath();
        }
        for (String c : new String[] {"/usr/local/bin/iris", "/opt/homebrew/bin/iris"}) {
            if (new File(c).canExecute()) {
                return c;
            }
        }
        return "iris";
    }
}
