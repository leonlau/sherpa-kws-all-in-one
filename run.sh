# # Build Docker image
echo "[INFO] Building Docker image..."
docker build -t sherpa-kws-demo-builder .
# # Run container and copy output

sudo rm -rf ./output
rm ./output.tar.gz

echo "[INFO] Running build container..."
docker run --rm -v "$(pwd)/output:/host-output" sherpa-kws-demo-builder


tar -zcvf  output.tar.gz output

