package iris.intellij;

import com.intellij.execution.filters.ConsoleFilterProvider;
import com.intellij.execution.filters.Filter;
import com.intellij.execution.filters.OpenFileHyperlinkInfo;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.LocalFileSystem;
import com.intellij.openapi.vfs.VirtualFile;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Makes file.iris:LINE:COL in compiler errors and panics a link to that place. */
public final class IrisConsoleFilterProvider implements ConsoleFilterProvider {
    private static final Pattern PLACE = Pattern.compile("([^\\s:'\"()]+\\.iris):(\\d+)(?::(\\d+))?");

    @Override
    public Filter[] getDefaultFilters(Project project) {
        return new Filter[] {(line, entireLength) -> {
            Matcher m = PLACE.matcher(line);
            List<Filter.ResultItem> items = new ArrayList<>();
            int lineStart = entireLength - line.length();
            while (m.find()) {
                VirtualFile vf = find(project, m.group(1));
                if (vf == null) {
                    continue;
                }
                int ln = Integer.parseInt(m.group(2)) - 1;
                int col = m.group(3) == null ? 0 : Integer.parseInt(m.group(3)) - 1;
                items.add(new Filter.ResultItem(lineStart + m.start(), lineStart + m.end(),
                    new OpenFileHyperlinkInfo(project, vf, Math.max(ln, 0), Math.max(col, 0))));
            }
            return items.isEmpty() ? null : new Filter.Result(items);
        }};
    }

    private static VirtualFile find(Project project, String path) {
        File f = new File(path);
        if (!f.isAbsolute() && project.getBasePath() != null) {
            f = new File(project.getBasePath(), path);
        }
        return LocalFileSystem.getInstance().findFileByIoFile(f);
    }
}
