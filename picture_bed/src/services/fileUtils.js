import SparkMD5 from 'spark-md5';

export const calculateTextMD5 = (text) => {
  const spark = new SparkMD5();
  spark.append(text);
  return spark.end();
};

export const calculateFileMD5 = (file, chunkSize = 2 * 1024 * 1024) => {
  return new Promise((resolve, reject) => {
    const chunks = Math.ceil(file.size / chunkSize);
    let currentChunk = 0;
    const spark = new SparkMD5.ArrayBuffer();
    const reader = new FileReader();

    reader.onload = (e) => {
      spark.append(e.target.result);
      currentChunk++;
      if (currentChunk < chunks) {
        loadNext();
      } else {
        resolve(spark.end());
      }
    };
    reader.onerror = reject;

    function loadNext() {
      const start = currentChunk * chunkSize;
      const end = Math.min(start + chunkSize, file.size);
      reader.readAsArrayBuffer(file.slice(start, end));
    }

    loadNext();
  });
};
