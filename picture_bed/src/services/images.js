import { API_CONFIG } from '../config';
import { calculateFileMD5 } from './fileUtils';
import { isTokenExpiredCode, makeTokenExpiredError, normalizeStorageUrl, requestJson } from './http';

export const fetchUserImages = async (user) => {
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.MY_FILES}?cmd=normal`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({
      token: user.token,
      user: user.username,
      count: 20,
      start: 0
    })
  });

  if (data.code === 0) {
    return (data.files || []).map(file => ({
      ...file,
      name: file.file_name || file.filename,
      url: normalizeStorageUrl(file.url),
      pv: file.pv || 0,
    }));
  }
  if (isTokenExpiredCode(data.code)) {
    throw makeTokenExpiredError();
  }
  throw new Error(data.msg || '获取图片列表失败');
};

const tryInstantUpload = async (file, user, md5) => {
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.MD5}`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({
      user: user.username,
      token: user.token,
      md5,
      fileName: file.name
    })
  });

  if (isTokenExpiredCode(data.code)) {
    throw makeTokenExpiredError();
  }
  if (data.code === 0) {
    return { instant: true, alreadyExists: false, md5 };
  }
  if (data.code === 5) {
    return { instant: true, alreadyExists: true, md5 };
  }
  if (data.code === 1) {
    return { instant: false, alreadyExists: false, md5 };
  }

  throw new Error(data.msg || '秒传检测失败');
};

// 普通上传（小文件 <= 10MB）
export const uploadImage = async (file, user, onProgress) => {
  // 大文件自动走分片上传
  if (file.size > API_CONFIG.CHUNK_THRESHOLD) {
    return uploadChunked(file, user, onProgress);
  }

  const md5 = await calculateFileMD5(file);
  const instantResult = await tryInstantUpload(file, user, md5);
  if (instantResult.instant) {
    if (onProgress) onProgress(100);
    return instantResult;
  }

  // FormData 字段顺序必须匹配后端解析顺序：
  // file 在前（含 filename），然后 user、token、md5、size 在后
  const formData = new FormData();
  formData.append('file', file);
  formData.append('user', user.username);
  formData.append('token', user.token);
  formData.append('md5', md5);
  formData.append('size', file.size);

  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.UPLOAD}`, {
    method: 'POST',
    body: formData
  });

  if (onProgress) onProgress(100);

  if (isTokenExpiredCode(data.code)) {
    throw makeTokenExpiredError();
  }
  if (data.code !== 0) {
    throw new Error(data.msg || '上传失败');
  }
  return { ...data, instant: false, alreadyExists: false, md5 };
};

// 分片上传（大文件 > 10MB）
export const uploadChunked = async (file, user, onProgress) => {
  const md5 = await calculateFileMD5(file);
  const instantResult = await tryInstantUpload(file, user, md5);
  if (instantResult.instant) {
    if (onProgress) onProgress(100);
    return instantResult;
  }

  const chunkSize = API_CONFIG.CHUNK_SIZE;
  const chunkCount = Math.ceil(file.size / chunkSize);

  if (onProgress) onProgress(0);

  // Step 1: 初始化分片上传
  const initData = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.CHUNK_INIT}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      user: user.username,
      token: user.token,
      filename: file.name,
      md5: md5,
      size: file.size,
      chunkCount: chunkCount
    })
  });

  if (isTokenExpiredCode(initData.code)) {
    throw makeTokenExpiredError();
  }
  if (initData.code !== 0) {
    throw new Error(initData.msg || '分片初始化失败');
  }

  // 获取已上传的分片索引（断点续传）
  const uploadedSet = new Set();
  const uploadedChunks = initData.uploadedChunks || initData.uploaded || '';
  if (uploadedChunks.length > 0) {
    uploadedChunks.split(',').forEach(idx => {
      const n = parseInt(idx.trim(), 10);
      if (!isNaN(n)) uploadedSet.add(n);
    });
  }

  // Step 2: 逐个上传分片
  let completedChunks = uploadedSet.size;
  for (let i = 0; i < chunkCount; i++) {
    // 跳过已上传的分片
    if (uploadedSet.has(i)) {
      continue;
    }

    const start = i * chunkSize;
    const end = Math.min(start + chunkSize, file.size);
    const chunk = file.slice(start, end);

    const uploadUrl = `${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.CHUNK_UPLOAD}?md5=${encodeURIComponent(md5)}&index=${i}&user=${encodeURIComponent(user.username)}&token=${encodeURIComponent(user.token)}`;

    const uploadData = await requestJson(uploadUrl, {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: chunk
    });

    if (uploadData.code !== 0) {
      throw new Error(`分片 ${i} 上传失败`);
    }

    completedChunks++;
    if (onProgress) {
      // 分片上传占 90%，合并占 10%
      onProgress(Math.round((completedChunks / chunkCount) * 90));
    }
  }

  // Step 3: 请求合并
  const mergeData = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.CHUNK_MERGE}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      user: user.username,
      token: user.token,
      md5: md5,
      filename: file.name
    })
  });

  if (isTokenExpiredCode(mergeData.code)) {
    throw makeTokenExpiredError();
  }
  if (mergeData.code !== 0) {
    throw new Error(mergeData.msg || '分片合并失败');
  }

  if (onProgress) onProgress(100);
  return { ...mergeData, instant: false, alreadyExists: false, md5 };
};

// 更新文件下载次数（pv+1）
export const pvFile = async (image, user) => {
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.DEAL_FILE}?cmd=pv`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({
      token: user.token,
      user: user.username,
      md5: image.md5,
      filename: image.file_name || image.name
    })
  });

  if (data.code !== 0) {
    throw new Error(data.msg || 'pv更新失败');
  }
  return data;
};

export const shareFile = async (image, user) => {
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.DEAL_FILE}?cmd=share`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({
      token: user.token,
      user: user.username,
      md5: image.md5,
      filename: image.file_name
    })
  });

  if (data.code !== 0) {
    throw new Error(data.msg || '分享失败');
  }
  return data;
};

export const cancelShareFile = async (image, user) => {
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.DEAL_SHARE_FILE}?cmd=cancel`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({
      user: user.username,
      token: user.token,
      md5: image.md5,
      filename: image.file_name
    })
  });

  if (data.code !== 0) {
    throw new Error(data.msg || '取消分享失败');
  }
  return data;
};

export const deleteImage = async (image, user) => {
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.DEAL_FILE}?cmd=del`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({
      token: user.token,
      user: user.username,
      md5: image.md5,
      filename: image.file_name
    })
  });

  if (data.code !== 0) {
    throw new Error(data.msg || '删除失败');
  }
  return data;
};
