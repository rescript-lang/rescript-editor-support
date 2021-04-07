open Infix;
open TopTypes;

let escapePreprocessingFlags = flag =>
  /* ppx escaping not supported on windows yet */
  if (Sys.os_type == "Win32") {
    flag;
  } else {
    let parts = Utils.split_on_char(' ', flag);
    switch (parts) {
    | [("-ppx" | "-pp") as flag, ...rest] =>
      flag ++ " " ++ Utils.maybeQuoteFilename(String.concat(" ", rest))
    | _ => flag
    };
  };

/**
 * Creates the `pathsForModule` hashtbl, which maps a `moduleName` to it's `paths` (the ml/re, mli/rei, cmt, and cmti files)
 */
let makePathsForModule =
    (
      localModules: list((string, SharedTypes.paths)),
      dependencyModules: list((string, SharedTypes.paths)),
    ) => {
  let pathsForModule = Hashtbl.create(30);

  dependencyModules
  |> List.iter(((modName, paths)) => {
       Hashtbl.replace(pathsForModule, modName, paths)
     });

  localModules
  |> List.iter(((modName, paths)) => {
       Hashtbl.replace(pathsForModule, modName, paths)
     });

  pathsForModule;
};

let newBsPackage = rootPath =>
  switch (Files.readFileResult(rootPath /+ "bsconfig.json")) {
  | Error(e) => Error(e)
  | Ok(raw) =>
    let config = Json.parse(raw);

    Log.log({|📣 📣 NEW BSB PACKAGE 📣 📣|});
    /* failwith("Wat"); */
    Log.log("- location: " ++ rootPath);

    let compiledBase = BuildSystem.getCompiledBase(rootPath);
    switch (FindFiles.findDependencyFiles(~debug=true, rootPath, config)) {
    | Error(e) => Error(e)
    | Ok((dependencyDirectories, dependencyModules)) =>
      switch (
        compiledBase
        |> RResult.orError(
             "You need to run bsb first so that reason-language-server can access the compiled artifacts.\nOnce you've run bsb, restart the language server.",
           )
      ) {
      | Error(e) => Error(e)
      | Ok(compiledBase) =>
        Ok(
          {
            let namespace = FindFiles.getNamespace(config);
            let localSourceDirs =
              FindFiles.getSourceDirectories(
                ~includeDev=true,
                rootPath,
                config,
              );
            Log.log(
              "Got source directories "
              ++ String.concat(" - ", localSourceDirs),
            );
            let localModules =
              FindFiles.findProjectFiles(
                ~debug=true,
                namespace,
                rootPath,
                localSourceDirs,
                compiledBase,
              );
            /* |> List.map(((name, paths)) => (switch (namespace) {
               | None => name
               | Some(n) => name ++ "-" ++ n }, paths)); */
            Log.log(
              "-- All local modules found: "
              ++ string_of_int(List.length(localModules)),
            );
            localModules
            |> List.iter(((name, paths)) => {
                 Log.log(name);
                 switch (paths) {
                 | SharedTypes.Impl(cmt, _) => Log.log("impl " ++ cmt)
                 | Intf(cmi, _) => Log.log("intf " ++ cmi)
                 | _ => Log.log("Both")
                 };
               });

            let pathsForModule =
              makePathsForModule(localModules, dependencyModules);

            let opens =
              switch (namespace) {
              | None => []
              | Some(namespace) =>
                let cmt = compiledBase /+ namespace ++ ".cmt";
                Log.log(
                  "############ Namespaced as " ++ namespace ++ " at " ++ cmt,
                );
                Hashtbl.add(pathsForModule, namespace, Impl(cmt, None));
                [FindFiles.nameSpaceToName(namespace)];
              };
            Log.log(
              "Dependency dirs " ++ String.concat(" ", dependencyDirectories),
            );

            let opens = {
              let flags =
                MerlinFile.getFlags(rootPath)
                |> RResult.withDefault([""])
                |> List.map(escapePreprocessingFlags);
              let opens =
                List.fold_left(
                  (opens, item) => {
                    let parts = Utils.split_on_char(' ', item);
                    let rec loop = items =>
                      switch (items) {
                      | ["-open", name, ...rest] => [name, ...loop(rest)]
                      | [_, ...rest] => loop(rest)
                      | [] => []
                      };
                    opens @ loop(parts);
                  },
                  opens,
                  flags,
                );
              opens;
            };

            let interModuleDependencies =
              Hashtbl.create(List.length(localModules));

            {
              rootPath,
              localModules: localModules |> List.map(fst),
              dependencyModules: dependencyModules |> List.map(fst),
              pathsForModule,
              opens,
              namespace,
              interModuleDependencies,
            };
          },
        )
      }
    };
  };

let findRoot = (~uri, packagesByRoot) => {
  let path = Uri2.toPath(uri);
  let rec loop = path =>
    if (path == "/") {
      None;
    } else if (Hashtbl.mem(packagesByRoot, path)) {
      Some(`Root(path));
    } else if (Files.exists(path /+ "bsconfig.json")) {
      Some(`Bs(path));
    } else {
      loop(Filename.dirname(path));
    };
  loop(Filename.dirname(path));
};

let getPackage = (~uri, state) =>
  if (Hashtbl.mem(state.rootForUri, uri)) {
    Ok(
      Hashtbl.find(
        state.packagesByRoot,
        Hashtbl.find(state.rootForUri, uri),
      ),
    );
  } else {
    switch (
      findRoot(~uri, state.packagesByRoot)
      |> RResult.orError("No root directory found")
    ) {
    | Error(e) => Error(e)
    | Ok(root) =>
      switch (
        switch (root) {
        | `Root(rootPath) =>
          Hashtbl.replace(state.rootForUri, uri, rootPath);
          Ok(
            Hashtbl.find(
              state.packagesByRoot,
              Hashtbl.find(state.rootForUri, uri),
            ),
          );
        | `Bs(rootPath) =>
          switch (newBsPackage(rootPath)) {
          | Error(e) => Error(e)
          | Ok(package) =>
            Hashtbl.replace(state.rootForUri, uri, package.rootPath);
            Hashtbl.replace(state.packagesByRoot, package.rootPath, package);
            Ok(package);
          }
        }
      ) {
      | Error(e) => Error(e)
      | Ok(package) => Ok(package)
      }
    };
  };
