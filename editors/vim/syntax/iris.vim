" Iris syntax for Vim and Neovim.
if exists("b:current_syntax")
  finish
endif

syn keyword irisKeyword if else for in match break continue return try catch defer errdefer scoped
syn keyword irisKeyword spawn select thread when comptime import pub const test asm
syn keyword irisOperator not and or is
syn keyword irisStructure struct union enum interface type dyn chan threadlocal
syn keyword irisType void bool i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 str String Error Duration
syn keyword irisType u16be u32be u64be i16be i32be i64be
syn keyword irisType Mutex Thread Atomic List Map
syn keyword irisBoolean true false
syn keyword irisConstant null
syn keyword irisSelf self
syn keyword irisBuiltin print panic format alloc free sizeof error sleep after expect

" a name followed by ( is a function, called or declared; a name with a
" capital first letter and a small letter in it is a type: Request, NetError
syn match irisFunction "\<\h\w*\ze("
syn match irisUserType "\<[A-Z]\w*[a-z]\w*\>"
" an enum value or a type's value after a dot: .tcp, .{x: 1}
syn match irisEnumDot +\%(\w\|)\|]\|\.\|"\)\@<!\.\h\w*+

syn match irisNumber "\<0x[0-9a-fA-F_]\+\>"
syn match irisNumber "\<0b[01_]\+\>"
syn match irisNumber "\<[0-9][0-9_]*\(\.[0-9][0-9_]*\)\?\([eE][+-]\?[0-9]\+\)\?\(ns\|us\|ms\|s\|m\|h\|KiB\|MiB\|GiB\)\?\>"
syn match irisAttribute "@[a-zA-Z_][a-zA-Z0-9_]*"
syn match irisChar "'\(\\x[0-9a-fA-F]\{2}\|\\.\|[^'\\]\)'" contains=irisEscape

syn region irisInterp contained matchgroup=irisInterpDelim start="{" end="}" contains=TOP
syn match irisEscape contained "\\\(x[0-9a-fA-F]\{2}\|u{[0-9a-fA-F]\+}\|.\)"
syn match irisBraces contained "{{\|}}"
syn region irisString start=+"+ skip=+\\\\\|\\"+ end=+"+ contains=irisEscape,irisBraces,irisInterp oneline
syn region irisString start=+"""+ end=+"""+ contains=irisEscape,irisBraces,irisInterp
syn region irisRawString start=+r"""+ end=+"""+
syn region irisRawString start=+r"+ end=+"+ oneline

syn keyword irisTodo contained TODO FIXME XXX NOTE
syn region irisComment start="//" end="$" contains=irisTodo,@Spell
syn region irisComment start="/\*" end="\*/" contains=irisTodo,@Spell

" code blocks fold, and so do struct and enum bodies
syn region irisBlockBrace start="{" end="}" transparent fold contains=TOP

hi def link irisKeyword Keyword
hi def link irisOperator Operator
hi def link irisStructure Structure
hi def link irisType Type
hi def link irisUserType Type
hi def link irisBoolean Boolean
hi def link irisConstant Constant
hi def link irisSelf Special
hi def link irisBuiltin Function
hi def link irisFunction Function
hi def link irisEnumDot Constant
hi def link irisNumber Number
hi def link irisAttribute PreProc
hi def link irisChar Character
hi def link irisString String
hi def link irisRawString String
hi def link irisEscape SpecialChar
hi def link irisBraces SpecialChar
hi def link irisInterpDelim Delimiter
hi def link irisComment Comment
hi def link irisTodo Todo

let b:current_syntax = "iris"
