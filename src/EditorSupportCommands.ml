let posOfLexing {Lexing.pos_lnum; pos_cnum; pos_bol} =
  Json.Object [("line", Json.Number (float_of_int (pos_lnum - 1))); ("character", Json.Number (float_of_int (pos_cnum - pos_bol)))]

let rangeOfLoc {Location.loc_start; loc_end} =
  Json.Object [("start", posOfLexing loc_start); ("end", posOfLexing loc_end)]

let dumpLocations state ~package ~file ~extra ~selectPos uri =
  let locations =
    extra.SharedTypes.locations
    |> List.filter (fun (l, _) -> not l.Location.loc_ghost)
  in
  let locations =
    match selectPos with
    | Some pos -> (
      let pos = Utils.cmtLocFromVscode pos in
      match References.locForPos ~extra:{extra with locations} pos with
      | None -> []
      | Some l -> [l] )
    | None -> locations
  in
  let dedupTable = Hashtbl.create 1 in
  let dedupHover hover i =
    let isCandidate = String.length hover > 10 in
    if isCandidate then (
      match Hashtbl.find_opt dedupTable hover with
      | Some n -> Json.String ("#" ^ string_of_int n)
      | None ->
        Hashtbl.replace dedupTable hover i;
        Json.String hover )
    else Json.String hover
  in
  let locationsInfo =
    locations
    |> Utils.filterMapIndex (fun i ((location : Location.t), loc) ->
        let locIsModule =
          match loc with
          | SharedTypes.LModule _ | TopLevelModule _ -> true
          | TypeDefinition _ | Typed _ | Constant _ | Explanation _ -> false
        in
        let hoverText =
          Hover.newHover ~file
            ~getModule:(State.fileForModule state ~package)
            loc
        in
        let hover =
          match hoverText with
          | None -> []
          | Some s -> [("hover", dedupHover s i)]
        in
        let uriLocOpt =
          References.definitionForLoc ~pathsForModule:package.pathsForModule
            ~file ~getUri:(State.fileForUri state)
            ~getModule:(State.fileForModule state ~package)
            loc
        in
        let def, skipZero =
          match uriLocOpt with
          | None -> ([], false)
          | Some (uri2, loc) ->
            let uriIsCurrentFile = uri = uri2 in
            let posIsZero {Lexing.pos_lnum; pos_bol; pos_cnum} =
              pos_lnum = 1 && pos_cnum - pos_bol = 0
            in
            (* Skip if range is all zero, unless it's a module *)
            let skipZero =
              (not locIsModule) && loc.loc_start |> posIsZero
              && loc.loc_end |> posIsZero
            in
            let range = ("range", rangeOfLoc loc) in
            (
              [
                ("definition",
                  Json.Object
                    (match uriIsCurrentFile with
                    | true -> [range]
                    | false -> [("uri", Json.String (Uri2.toString uri2)); range])
                )
              ],
              skipZero
            )
        in
        let skip = skipZero || (hover = [] && def = []) in
        match skip with
        | true -> None
        | false -> Some (Json.Object ([("range", rangeOfLoc location)] @ hover @ def)))
  in
  Json.stringify (Json.Array locationsInfo)

(* Split (line,char) from filepath:line:char *)
let splitLineChar pathWithPos =
  let mkPos line char = Some (line |> int_of_string, char |> int_of_string) in
  match pathWithPos |> String.split_on_char ':' with
  | [filePath; line; char] -> (filePath, mkPos line char)
  | [drive; rest; line; char] ->
    (* c:\... on Windows *)
    (drive ^ ":" ^ rest, mkPos line char)
  | _ -> (pathWithPos, None)

let dump files =
  Shared.cacheTypeToString := true;
  let state = TopTypes.empty () in
  files
  |> List.iter (fun pathWithPos ->
      let filePath, selectPos = pathWithPos |> splitLineChar in
      let filePath = Files.maybeConcat (Unix.getcwd ()) filePath in
      let uri = Uri2.fromPath filePath in
      let result =
        match State.getFullFromCmt ~state ~uri with
        | Error message ->
          prerr_endline message;
          "[]"
        | Ok (package, {file; extra}) ->
          dumpLocations state ~package ~file ~extra ~selectPos uri
      in
      print_endline result)

let complete ~path ~line ~char ~currentFile =
  let state = TopTypes.empty () in
  let filePath = Files.maybeConcat (Unix.getcwd ()) path in
  let uri = Uri2.fromPath filePath in
  let result =
    match State.getFullFromCmt ~state ~uri with
    | Error message ->
      prerr_endline message;
      "[]"
    | Ok (package, full) ->
      let maybeText = Files.readFile currentFile in
      NewCompletions.computeCompletions ~full ~maybeText ~package ~pos:(line, char) ~state
      |> List.map Protocol.stringifyCompletionItem
      |> Protocol.array
  in
  print_endline result

let hover state ~file ~line ~char ~extra ~package =
  let locations =
    extra.SharedTypes.locations
    |> List.filter (fun (l, _) -> not l.Location.loc_ghost)
  in
  let locations =
    let pos = (line, char) in
    let pos = Utils.cmtLocFromVscode pos in
    match References.locForPos ~extra:{extra with locations} pos with
    | None -> []
    | Some l -> [l]
  in
  let locationsInfo =
    locations
    |> Utils.filterMap (fun (_, loc) ->
        let locIsModule =
          match loc with
          | SharedTypes.LModule _ | TopLevelModule _ -> true
          | TypeDefinition _ | Typed _ | Constant _ | Explanation _ -> false
        in
        let hoverText =
          Hover.newHover ~file
            ~getModule:(State.fileForModule state ~package)
            loc
        in
        let hover =
          match hoverText with
          | None -> None
          | Some s ->
            let open Protocol in
            Some {
              contents = s
            }
        in
        let uriLocOpt =
          References.definitionForLoc ~pathsForModule:package.pathsForModule
            ~file ~getUri:(State.fileForUri state)
            ~getModule:(State.fileForModule state ~package)
            loc
        in
        let skipZero =
          match uriLocOpt with
          | None -> false
          | Some (_, loc) ->
            let posIsZero {Lexing.pos_lnum; pos_bol; pos_cnum} =
              pos_lnum = 1 && pos_cnum - pos_bol = 0
            in
            (* Skip if range is all zero, unless it's a module *)
            (not locIsModule) && loc.loc_start |> posIsZero
            && loc.loc_end |> posIsZero
        in
        match hover with
        | Some hover when not skipZero -> Some hover
        | _ -> None
      )
  in
  match locationsInfo with
  | [] -> Protocol.null
  | head :: _ -> Protocol.stringifyHover head

let hover ~path ~line ~char =
  let state = TopTypes.empty () in
  let filePath = Files.maybeConcat (Unix.getcwd ()) path in
  let uri = Uri2.fromPath filePath in
  let result =
    match State.getFullFromCmt ~state ~uri with
    | Error message ->
      Protocol.stringifyHover {contents = message}
    | Ok (package, {file; extra}) ->
      hover state ~file ~line ~char ~extra ~package
  in
  print_endline result
