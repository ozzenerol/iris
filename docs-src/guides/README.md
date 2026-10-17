# How guides are written

A guide builds one whole program, from an empty folder to the finished program, in steps the reader types along with. Someone who knows nothing but the Tour can follow it without guessing. These rules hold for every guide in this folder. This file isn't a page of the site.

## From nothing

1. The first section, "Before you start", lists everything the guide needs: Iris, and every other program or library (SQLite, curl), with the command that checks each one is there.
2. The guide then makes the folder and the file: `mkdir notes`, `cd notes`, and which file to create. Every file the program uses is made by the guide or by the program itself, at the step that first needs it.
3. Nothing is used before it exists. A query comes after the code that opens the database and makes the table; a function is called only once it is in the file; a variable is set only after it is declared.

## Every step runs

4. Each step ends with a program that compiles and runs. No step leaves the file half done, and no step waits for a later one to work.
5. Each step shows every declaration it adds or changes in full: whole structs, whole functions, the whole `import` line. Never a fragment, never "the rest stays the same".
6. Each step ends with the whole file as it is now, in an `iris step` block, which the page shows folded under "main.iris after this step". A reader who got lost copies it and goes on.
7. Each step says how to run the program and what to type to try it, and shows what comes out. A server is started in one terminal (a `shell start` block) and tried from another (a `shell try` block, followed by its `output` block).

## Tested

8. The test suite follows every guide as a reader would, in one folder: it checks each step's file with `iris check`, starts the program with the step's `shell start` block, runs each `shell try` block, and compares what it prints with the `output` block after it. In an `output` block, `…` stands for text that changes from run to run, like times and dates.
9. The last step's file is the whole program. "The whole program" section shows it once more, unfolded, and the suite checks it is the same text.

## Plain

10. The guide says what each new thing does when it first appears, and links to the Tour chapter or the library page for the details.
11. A guide calls the API from the program's code directly. It shows Iris only, and never compares it with other languages.
12. It ends with what was built and where to go next.
