{
  description = "Logos Execution Zone Indexer Module (universal core, logos-module-builder)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    logos-execution-zone.url = "git+https://github.com/logos-blockchain/logos-execution-zone?ref=main";
  };

  outputs =
    inputs@{ logos-module-builder, logos-execution-zone, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      externalLibInputs = {
        # Structured form: the dep exposes its lib under packages.<system>.indexer
        # (not .default), so map the default build variant to that package.
        indexer_ffi = {
          input = logos-execution-zone;
          packages = {
            default = "indexer";
          };
        };
      };
    };
}
