import { API_CONFIG } from '../config';
import { normalizeStorageUrl, requestJson } from './http';

// 获取共享文件列表
export const fetchSharedFiles = async (start = 0, count = 50) => {
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.SHARE_FILES}?cmd=normal`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({ start, count })
  });

  if (data.code === 0) {
    return {
      files: (data.files || []).map(file => ({
        ...file,
        name: file.file_name,
        url: normalizeStorageUrl(file.url),
      })),
      total: data.total || 0
    };
  }
  throw new Error('获取共享文件列表失败');
};

// 获取共享文件排行榜（按下载量降序）
export const fetchSharedFilesRanking = async (start = 0, count = 50) => {
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.SHARE_FILES}?cmd=pvdesc`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({ start, count })
  });

  if (data.code === 0) {
    return {
      files: (data.files || []).map(file => ({
        ...file,
        url: normalizeStorageUrl(file.url),
      })),
      total: data.total || 0
    };
  }
  throw new Error('获取排行榜失败');
};

// 转存共享文件到自己的文件列表
export const saveSharedFile = async (file, user) => {
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.DEAL_SHARE_FILE}?cmd=save`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({
      user: user.username,
      token: user.token,
      md5: file.md5,
      filename: file.file_name
    })
  });

  if (data.code === 5) {
    throw new Error('文件已存在');
  }
  if (data.code !== 0) {
    throw new Error(data.msg || '转存失败');
  }
  return data;
};

// 共享文件下载计数
export const pvSharedFile = async (file, user) => {
  if (!user || !user.token) return null;
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.DEAL_SHARE_FILE}?cmd=pv`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({
      user: file.user,
      token: user.token,
      md5: file.md5,
      filename: file.file_name
    })
  });

  if (data.code !== 0) {
    throw new Error(data.msg || 'pv更新失败');
  }
  return data;
};
