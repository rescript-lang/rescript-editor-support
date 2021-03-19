open SharedTypes;

let debugReferences = ref(true);

let maybeLog = m =>
  if (debugReferences^) {
    Log.log("[ref] " ++ m);
  };

let checkPos =
    (
      (line, char),
      {Location.loc_start: {pos_lnum, pos_bol, pos_cnum}, loc_end},
    ) =>
  if (line < pos_lnum || line == pos_lnum && char < pos_cnum - pos_bol) {
    false;
  } else if (line > loc_end.pos_lnum
             || line == loc_end.pos_lnum
             && char > loc_end.pos_cnum
             - loc_end.pos_bol) {
    false;
  } else {
    true;
  };

let locsForPos = (~extra, pos) => {
  extra.locations |> List.filter(((loc, _l)) => checkPos(pos, loc));
};

let locForPos = (~extra, pos) => {
  switch (locsForPos(~extra, pos)) {
  | [
      (loc1, Typed(_, LocalReference(_))),
      (
        loc2,
        Typed(_, GlobalReference("Js_OO", Tip("unsafe_downgrade"), _)),
      ),
      (loc3, _) as l3,
    ]
      when loc1 == loc2 && loc2 == loc3 =>
    // JSX and compiler combined:
    // ~x becomes Js_OO.unsafe_downgrade(Props)#x
    // heuristic for: [Props, unsafe_downgrade, x], give loc of `x`
    Some(l3)
  | [(loc1, _), (loc2, _) as l, (loc3, _)]
      when loc1 == loc2 && loc2 == loc3 =>
    // JSX with at most one child
    // heuristic for: [makeProps, make, createElement], give the loc of `make`
    Some(l)
  | [(loc1, _), (loc2, _), (loc3, _) as l, (loc4, _)]
      when loc1 == loc2 && loc2 == loc3 && loc3 == loc4 =>
    // JSX variadic, e.g. <C> {x} {y} </C>
    // heuristic for: [makeProps, React.null, make, createElementVariadic], give the loc of `make`
    Some(l)
  | [l, ..._] => Some(l)
  | _ => None
  };
};

/** Other locations *within this file* that refer to the same thing.
 *
 * Useful for "highlight" stuff. */
let localReferencesForLoc = (~file, ~extra, loc) =>
  switch (loc) {
  | Explanation(_)
  | Typed(_, NotFound)
  | LModule(NotFound)
  | TopLevelModule(_)
  | Constant(_) => None
  | TypeDefinition(_, _, stamp) =>
    Hashtbl.find_opt(extra.internalReferences, stamp)
  | LModule(LocalReference(stamp, tip) | Definition(stamp, tip))
  | Typed(_, LocalReference(stamp, tip) | Definition(stamp, tip)) =>
    open Infix;
    let%opt localStamp =
      switch (tip) {
      | Constructor(name) =>
        Query.getConstructor(file, stamp, name) |?>> (x => x.stamp)
      | Field(name) => Query.getField(file, stamp, name) |?>> (x => x.stamp)
      | _ => Some(stamp)
      };
    Hashtbl.find_opt(extra.internalReferences, localStamp);
  | LModule(GlobalReference(moduleName, path, tip))
  | Typed(_, GlobalReference(moduleName, path, tip)) =>
    switch (Hashtbl.find_opt(extra.externalReferences, moduleName)) {
    | None => None
    | Some(refs) =>
      Some(
        refs
        |> Utils.filterMap(((p, t, l)) =>
             p == path && t == tip ? Some(l) : None
           ),
      )
    }
  };

let definedForLoc = (~file, ~getModule, locKind) => {
  let inner = (~file, stamp, tip) => {
    switch (tip) {
    | Constructor(name) =>
      let%opt constructor = Query.getConstructor(file, stamp, name);
      Some((None, None, file, `Constructor(constructor)));
    | Field(name) =>
      let%opt field = Query.getField(file, stamp, name);
      Some((None, None, file, `Field(field)));
    | _ =>
      maybeLog(
        "Trying for declared "
        ++ tipToString(tip)
        ++ " "
        ++ string_of_int(stamp)
        ++ " in file "
        ++ Uri2.toString(file.uri),
      );
      let%opt declared =
        Query.declaredForTip(~stamps=file.stamps, stamp, tip);
      Some((declared.deprecated, declared.docstring, file, `Declared));
    };
  };

  switch (locKind) {
  | NotFound => None
  | LocalReference(stamp, tip)
  | Definition(stamp, tip) => inner(~file, stamp, tip)
  | GlobalReference(moduleName, path, tip) =>
    {
      maybeLog("Getting global " ++ moduleName);
      let%try file =
        getModule(moduleName)
        |> RResult.orError("Cannot get module " ++ moduleName);
      let env = Query.fileEnv(file);
      let%try (env, name) =
        Query.resolvePath(~env, ~path, ~getModule)
        |> RResult.orError("Cannot resolve path " ++ pathToString(path));
      let%try stamp =
        Query.exportedForTip(~env, name, tip)
        |> RResult.orError(
             "Exported not found for tip "
             ++ name
             ++ " > "
             ++ tipToString(tip),
           );
      maybeLog("Getting for " ++ string_of_int(stamp) ++ " in " ++ name);
      let%try res =
        inner(~file=env.file, stamp, tip)
        |> RResult.orError("could not get defined");
      maybeLog("Yes!! got it");
      Ok(res);
    }
    |> RResult.toOptionAndLog
  };
};

let alternateDeclared = (~file, ~pathsForModule, ~getUri, declared, tip) => {
  let%opt paths = Hashtbl.find_opt(pathsForModule, file.moduleName);
  maybeLog("paths for " ++ file.moduleName);
  switch (paths) {
  | IntfAndImpl(_, intf, _, impl) =>
    maybeLog("Have both!!");
    let intfUri = Uri2.fromPath(intf);
    let implUri = Uri2.fromPath(impl);
    if (intfUri == file.uri) {
      let%opt (file, extra) = getUri(implUri) |> RResult.toOptionAndLog;
      let%opt declared =
        Query.declaredForExportedTip(
          ~stamps=file.stamps,
          ~exported=file.contents.exported,
          declared.name.txt,
          tip,
        );
      Some((file, extra, declared));
    } else {
      let%opt (file, extra) = getUri(intfUri) |> RResult.toOptionAndLog;
      let%opt declared =
        Query.declaredForExportedTip(
          ~stamps=file.stamps,
          ~exported=file.contents.exported,
          declared.name.txt,
          tip,
        );
      Some((file, extra, declared));
    };
  | _ => None
  };
};

let resolveModuleReference =
    (~file, ~getModule, declared: declared(moduleKind)) => {
  switch (declared.item) {
  | Structure(_) => Some((file, Some(declared)))
  | Ident(path) =>
    let env = Query.fileEnv(file);
    switch (Query.fromCompilerPath(~env, path)) {
    | `Not_found => None
    | `Exported(env, name) =>
      let%opt stamp = Hashtbl.find_opt(env.exported.modules, name);
      let%opt md = Hashtbl.find_opt(env.file.stamps.modules, stamp);
      Some((env.file, Some(md)));
    /* Some((env.file.uri, validateLoc(md.name.loc, md.extentLoc))) */
    | `Global(moduleName, path) =>
      let%opt file = getModule(moduleName);
      let env = Query.fileEnv(file);
      let%opt (env, name) = Query.resolvePath(~env, ~getModule, ~path);
      let%opt stamp = Hashtbl.find_opt(env.exported.modules, name);
      let%opt md = Hashtbl.find_opt(env.file.stamps.modules, stamp);
      Some((env.file, Some(md)));
    /* Some((env.file.uri, validateLoc(md.name.loc, md.extentLoc))) */
    | `Stamp(stamp) =>
      let%opt md = Hashtbl.find_opt(file.stamps.modules, stamp);
      Some((file, Some(md)));
    /* Some((file.uri, validateLoc(md.name.loc, md.extentLoc))) */
    | `GlobalMod(name) =>
      let%opt file = getModule(name);
      /* maybeLog("Congrats, found a global mod"); */
      Some((file, None));
    | _ => None
    };
  };
};

let forLocalStamp =
    (
      ~pathsForModule,
      ~file,
      ~extra,
      ~allModules,
      ~getModule,
      ~getUri,
      ~getExtra,
      stamp,
      tip,
    ) => {
  let env = Query.fileEnv(file);
  open Infix;
  let%opt localStamp =
    switch (tip) {
    | Constructor(name) =>
      Query.getConstructor(file, stamp, name) |?>> (x => x.stamp)
    | Field(name) => Query.getField(file, stamp, name) |?>> (x => x.stamp)
    | _ => Some(stamp)
    };
  let%opt local = Hashtbl.find_opt(extra.internalReferences, localStamp);
  open Infix;
  let externals =
    {
      maybeLog("Checking externals: " ++ string_of_int(stamp));
      let%opt declared =
        Query.declaredForTip(~stamps=env.file.stamps, stamp, tip);
      if (isVisible(declared)) {
        /**
      if this file has a corresponding interface or implementation file
      also find the references in that file.
       */
        let alternativeReferences =
          {
            let%opt (file, extra, {stamp}) =
              alternateDeclared(
                ~pathsForModule,
                ~file,
                ~getUri,
                declared,
                tip,
              );
            let%opt localStamp =
              switch (tip) {
              | Constructor(name) =>
                Query.getConstructor(file, stamp, name) |?>> (x => x.stamp)
              | Field(name) =>
                Query.getField(file, stamp, name) |?>> (x => x.stamp)
              | _ => Some(stamp)
              };
            let%opt local =
              Hashtbl.find_opt(extra.internalReferences, localStamp);
            Some([(file.uri, local)]);
          }
          |? [];

        let%opt path =
          pathFromVisibility(declared.modulePath, declared.name.txt);
        maybeLog("Now checking path " ++ pathToString(path));
        let thisModuleName = file.moduleName;
        let externals =
          allModules
          |> List.filter(name => name != file.moduleName)
          |> Utils.filterMap(name =>
               {
                 let%try file =
                   getModule(name)
                   |> RResult.orError(
                        "Could not get file for module " ++ name,
                      );
                 let%try extra =
                   getExtra(name)
                   |> RResult.orError(
                        "Could not get extra for module " ++ name,
                      );
                 let%try refs =
                   Hashtbl.find_opt(extra.externalReferences, thisModuleName)
                   |> RResult.orError(
                        "No references in "
                        ++ name
                        ++ " for "
                        ++ thisModuleName,
                      );
                 let refs =
                   refs
                   |> Utils.filterMap(((p, t, l)) =>
                        p == path && t == tip ? Some(l) : None
                      );
                 Ok((file.uri, refs));
               }
               |> RResult.toOptionAndLog
             );
        Some(alternativeReferences @ externals);
      } else {
        maybeLog("Not visible");
        Some([]);
      };
    }
    |? [];
  Some([(file.uri, local), ...externals]);
};

let allReferencesForLoc =
    (
      ~pathsForModule,
      ~getUri,
      ~file,
      ~extra,
      ~allModules,
      ~getModule,
      ~getExtra,
      loc,
    ) => {
  switch (loc) {
  | Explanation(_)
  | Typed(_, NotFound)
  | LModule(NotFound)
  | TopLevelModule(_)
  | Constant(_) => Error("Not a valid loc")
  | TypeDefinition(_, _, stamp) =>
    forLocalStamp(
      ~pathsForModule,
      ~getUri,
      ~file,
      ~extra,
      ~allModules,
      ~getModule,
      ~getExtra,
      stamp,
      Type,
    )
    |> RResult.orError("Could not get for local stamp")
  | Typed(_, LocalReference(stamp, tip) | Definition(stamp, tip))
  | LModule(LocalReference(stamp, tip) | Definition(stamp, tip)) =>
    maybeLog(
      "Finding references for "
      ++ Uri2.toString(file.uri)
      ++ " and stamp "
      ++ string_of_int(stamp)
      ++ " and tip "
      ++ tipToString(tip),
    );
    forLocalStamp(
      ~pathsForModule,
      ~getUri,
      ~file,
      ~extra,
      ~allModules,
      ~getModule,
      ~getExtra,
      stamp,
      tip,
    )
    |> RResult.orError("Could not get for local stamp");
  | LModule(GlobalReference(moduleName, path, tip))
  | Typed(_, GlobalReference(moduleName, path, tip)) =>
    let%try file =
      getModule(moduleName)
      |> RResult.orError("Cannot get module " ++ moduleName);
    let env = Query.fileEnv(file);
    let%try (env, name) =
      Query.resolvePath(~env, ~path, ~getModule)
      |> RResult.orError("Cannot resolve path " ++ pathToString(path));
    let%try stamp =
      Query.exportedForTip(~env, name, tip)
      |> RResult.orError(
           "Exported not found for tip " ++ name ++ " > " ++ tipToString(tip),
         );
    let%try (file, extra) = getUri(env.file.uri);
    maybeLog(
      "Finding references for (global) "
      ++ Uri2.toString(env.file.uri)
      ++ " and stamp "
      ++ string_of_int(stamp)
      ++ " and tip "
      ++ tipToString(tip),
    );
    forLocalStamp(
      ~pathsForModule,
      ~getUri,
      ~file,
      ~extra,
      ~allModules,
      ~getModule,
      ~getExtra,
      stamp,
      tip,
    )
    |> RResult.orError("Could not get for local stamp");
  };
};

let refsForPos = (~file, ~extra, pos) => {
  let%opt (_, loc) = locForPos(~extra, pos);
  maybeLog("Got a loc for pos");
  let%opt refs = localReferencesForLoc(~file, ~extra, loc);
  Some(refs);
};

let validateLoc = (loc: Location.t, backup: Location.t) =>
  if (loc.loc_start.pos_cnum == (-1)) {
    if (backup.loc_start.pos_cnum == (-1)) {
      {
        Location.loc_ghost: true,
        loc_start: {
          pos_cnum: 0,
          pos_lnum: 1,
          pos_bol: 0,
          pos_fname: "",
        },
        loc_end: {
          pos_cnum: 0,
          pos_lnum: 1,
          pos_bol: 0,
          pos_fname: "",
        },
      };
    } else {
      backup;
    };
  } else {
    loc;
  };

let resolveModuleDefinition = (~file, ~getModule, stamp) => {
  let%opt md = Hashtbl.find_opt(file.stamps.modules, stamp);
  let%opt (file, declared) = resolveModuleReference(~file, ~getModule, md);
  let loc =
    switch (declared) {
    | None => Utils.topLoc(Uri2.toPath(file.uri))
    | Some(declared) => validateLoc(declared.name.loc, declared.extentLoc)
    };
  Some((file.uri, loc));
};

let definition = (~file, ~getModule, stamp, tip) => {
  switch (tip) {
  | Constructor(name) =>
    let%opt constructor = Query.getConstructor(file, stamp, name);
    Some((file.uri, constructor.cname.loc));
  | Field(name) =>
    let%opt field = Query.getField(file, stamp, name);
    Some((file.uri, field.fname.loc));
  | Module => resolveModuleDefinition(~file, ~getModule, stamp)
  | _ =>
    let%opt declared = Query.declaredForTip(~stamps=file.stamps, stamp, tip);
    let loc = validateLoc(declared.name.loc, declared.extentLoc);
    let env = Query.fileEnv(file);
    let uri = Query.getSourceUri(~env, ~getModule, declared.modulePath);
    maybeLog("Inner uri " ++ Uri2.toString(uri));
    Some((uri, loc));
  };
};

let orLog = (message, v) =>
  switch (v) {
  | None =>
    maybeLog(message);
    None;
  | _ => v
  };

let definitionForLoc = (~pathsForModule, ~file, ~getUri, ~getModule, loc) => {
  switch (loc) {
  | Typed(_, Definition(stamp, tip)) =>
    maybeLog("Trying to find a defintion for a definition");
    let%opt declared = Query.declaredForTip(~stamps=file.stamps, stamp, tip);
    maybeLog("Declared");
    if (declared.exported) {
      maybeLog("exported, looking for alternate " ++ file.moduleName);
      let%opt (file, _extra, declared) =
        alternateDeclared(~pathsForModule, ~file, ~getUri, declared, tip);
      let loc = validateLoc(declared.name.loc, declared.extentLoc);
      Some((file.uri, loc));
    } else {
      None;
    };
  | Explanation(_)
  | Typed(_, NotFound)
  | LModule(NotFound | Definition(_, _))
  | TypeDefinition(_, _, _)
  | Constant(_) => None
  | TopLevelModule(name) =>
    maybeLog("Toplevel " ++ name);
    open Infix;
    let%opt src =
      Hashtbl.find_opt(pathsForModule, name)
      |> orLog("No paths found")
      |?> getSrc
      |> orLog("No src found");
    Some((Uri2.fromPath(src), Utils.topLoc(src)));
  | LModule(LocalReference(stamp, tip))
  | Typed(_, LocalReference(stamp, tip)) =>
    maybeLog("Local defn " ++ tipToString(tip));
    definition(~file, ~getModule, stamp, tip);
  | LModule(GlobalReference(moduleName, path, tip))
  | Typed(_, GlobalReference(moduleName, path, tip)) =>
    maybeLog(
      "Global defn "
      ++ moduleName
      ++ " "
      ++ pathToString(path)
      ++ " : "
      ++ tipToString(tip),
    );
    let%opt file = getModule(moduleName);
    let env = Query.fileEnv(file);
    let%opt (env, name) = Query.resolvePath(~env, ~path, ~getModule);
    let%opt stamp = Query.exportedForTip(~env, name, tip);
    /** oooh wht do I do if the stamp is inside a pseudo-file? */
    maybeLog("Got stamp " ++ string_of_int(stamp));
    definition(~file=env.file, ~getModule, stamp, tip);
  };
};

let definitionForPos =
    (~pathsForModule, ~file, ~extra, ~getUri, ~getModule, pos) => {
  let%opt (_, loc) = locForPos(~extra, pos);
  maybeLog("Got a loc for pos");
  definitionForLoc(~pathsForModule, ~file, ~getUri, ~getModule, loc);
};
