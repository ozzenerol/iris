" Iris filetype settings for Vim and Neovim.
if exists("b:did_ftplugin")
  finish
endif
let b:did_ftplugin = 1

setlocal commentstring=//\ %s
setlocal comments=s1:/*,mb:*,ex:*/,://
setlocal formatoptions-=t formatoptions+=croql
setlocal expandtab shiftwidth=4 softtabstop=4 tabstop=4
setlocal smartindent
setlocal suffixesadd=.iris
setlocal include=^\\s*import

let b:undo_ftplugin = "setlocal commentstring< comments< formatoptions< expandtab<"
      \ . " shiftwidth< softtabstop< tabstop< smartindent< suffixesadd< include<"
