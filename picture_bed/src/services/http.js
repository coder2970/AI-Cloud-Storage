import { API_CONFIG } from '../config';

export const normalizeStorageUrl = (url) => {
  if (!url) return '';
  return url.replace(API_CONFIG.STORAGE_URL, API_CONFIG.BASE_URL);
};

export const makeTokenExpiredError = () => {
  const err = new Error('token验证失败');
  err.tokenExpired = true;
  return err;
};

export const isTokenExpiredCode = (code) => code === 4;

const parseResponseBody = async (response) => {
  const text = await response.text();
  if (!text) return {};

  try {
    return JSON.parse(text);
  } catch (error) {
    const err = new Error(`服务返回了非 JSON 响应: ${response.status}`);
    err.status = response.status;
    err.body = text;
    throw err;
  }
};

export const requestJson = async (url, options = {}) => {
  const response = await fetch(url, options);
  const data = await parseResponseBody(response);

  if (!response.ok) {
    const err = new Error(data.msg || data.message || `请求失败: ${response.status}`);
    err.status = response.status;
    err.body = data;
    throw err;
  }

  return data;
};
