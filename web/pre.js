// pre.js -- runs inside the Emscripten module before main().
// Mounts the persistent home directory and installs the game data that
// index.html downloaded (Module.etFiles: { 'etmain/pak0.pk3': Uint8Array }).
Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  FS.mkdirTree('/et/etmain');
  FS.mkdirTree('/home/et');
  FS.mount(IDBFS, {}, '/home/et');

  addRunDependency('et-idbfs');
  FS.syncfs(true, function (err) {
    if (err) console.warn('IDBFS load failed', err);
    removeRunDependency('et-idbfs');
  });

  var files = Module['etFiles'] || {};
  for (var name in files) {
    var dir = ('/et/' + name).replace(/\/[^\/]*$/, '');
    FS.mkdirTree(dir);
    FS.writeFile('/et/' + name, files[name], { canOwn: true });
  }
  Module['etFiles'] = null;
});
