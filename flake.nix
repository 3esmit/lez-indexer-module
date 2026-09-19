{
  description = "Logos Execution Zone Indexer Module (universal core, logos-module-builder)";

  # Pull pre-built artifacts from the self-hosted Logos Attic cache (Nix binary cache).
  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [ "public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU=" ];
  };

  inputs = {
    logos-module-builder.url = "github:3esmit/logos-module-builder?rev=324b459c3f7b59171d249f3ccbcc362403b3fcaf";
    # Keep the indexer on the Bedrock-compatible HTTP parser and Testnet FFI
    # profile. This revision builds the deployed Testnet clock/program IDs.
    logos-execution-zone.url = "github:3esmit/logos-execution-zone?rev=ae0bd578ef7b7ab7ff0f7d6d9d69fd2226d9647a";
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
