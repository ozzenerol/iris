package iris.intellij;

import com.intellij.extapi.psi.ASTWrapperPsiElement;
import com.intellij.lang.ASTNode;
import com.intellij.lang.ParserDefinition;
import com.intellij.lang.PsiParser;
import com.intellij.lexer.Lexer;
import com.intellij.openapi.project.Project;
import com.intellij.psi.FileViewProvider;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IFileElementType;
import com.intellij.psi.tree.TokenSet;

/** A flat tree: the file holds the tokens. Structure comes from iris lsp. */
public final class IrisParserDefinition implements ParserDefinition {
    @Override
    public Lexer createLexer(Project project) {
        return new IrisLexer();
    }

    @Override
    public PsiParser createParser(Project project) {
        return (root, builder) -> {
            var file = builder.mark();
            while (!builder.eof()) {
                builder.advanceLexer();
            }
            file.done(root);
            return builder.getTreeBuilt();
        };
    }

    @Override
    public IFileElementType getFileNodeType() {
        return IrisTokenTypes.FILE;
    }

    @Override
    public TokenSet getCommentTokens() {
        return IrisTokenTypes.COMMENTS;
    }

    @Override
    public TokenSet getStringLiteralElements() {
        return IrisTokenTypes.STRINGS;
    }

    @Override
    public PsiElement createElement(ASTNode node) {
        return new ASTWrapperPsiElement(node);
    }

    @Override
    public PsiFile createFile(FileViewProvider viewProvider) {
        return new IrisFile(viewProvider);
    }
}
